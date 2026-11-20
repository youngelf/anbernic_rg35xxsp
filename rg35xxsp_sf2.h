/*
 * rg35xxsp_sf2.h - Shared SoundFont 2.01 (.sf2) Stereo Audio Engine & Room Reverb
 *                  for Anbernic RG 35xxSP (aarch64 Cortex-A53) & Linux Desktop (x86_64)
 *
 * Wraps TinySoundFont (third_party/tsf.h) with:
 *   - Automatic SoundFont discovery (GeneralUser-GS.sf2 -> pocketband.sf2 -> system GM)
 *   - Preset / Drum-bank selection helpers (General MIDI bank 0 & percussion bank 128)
 *   - Stereo Schroeder/Freeverb-style room reverb (4 comb + 2 allpass diffusers per channel)
 *   - 20 Hz stereo DC-blocking high-pass filter & soft-clip master limiter (tanhf)
 *   - Zero dynamic allocations inside the real-time audio render loop
 *
 * Usage:
 *   In ONE .c file per application:
 *     #define RG35XXSP_SF2_IMPLEMENTATION
 *     #include "../rg35xxsp_sf2.h"
 *
 * All audio rendering calls MUST happen on the dedicated background audio thread.
 */
#ifndef RG35XXSP_SF2_H
#define RG35XXSP_SF2_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef RG35XXSP_SF2_IMPLEMENTATION
#define TSF_IMPLEMENTATION
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include "third_party/tsf.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* Stereo Room Reverb buffer lengths (mutually prime at 44.1 kHz, ~25..40 ms) */
#define SF2_REV_C0_L  1117
#define SF2_REV_C1_L  1277
#define SF2_REV_C2_L  1423
#define SF2_REV_C3_L  1613
#define SF2_REV_C0_R  1147
#define SF2_REV_C1_R  1307
#define SF2_REV_C2_R  1453
#define SF2_REV_C3_R  1643
#define SF2_REV_AP0   227
#define SF2_REV_AP1   557

typedef struct {
    tsf   *synth;
    int    sample_rate;
    float  reverb_wet;     /* 0.0 .. 0.45 */
    float  master_gain;    /* linear output gain, e.g. 1.35f */
    char   loaded_path[256];

    /* Stereo reverb delay lines */
    float  c0l[SF2_REV_C0_L], c1l[SF2_REV_C1_L], c2l[SF2_REV_C2_L], c3l[SF2_REV_C3_L];
    float  c0r[SF2_REV_C0_R], c1r[SF2_REV_C1_R], c2r[SF2_REV_C2_R], c3r[SF2_REV_C3_R];
    float  c0l_lp, c1l_lp, c2l_lp, c3l_lp;
    float  c0r_lp, c1r_lp, c2r_lp, c3r_lp;
    float  ap0l[SF2_REV_AP0], ap1l[SF2_REV_AP1];
    float  ap0r[SF2_REV_AP0], ap1r[SF2_REV_AP1];
    int    i0l, i1l, i2l, i3l;
    int    i0r, i1r, i2r, i3r;
    int    iap0, iap1;

    /* 20 Hz stereo DC blocker */
    float  dc_xl, dc_yl, dc_xr, dc_yr;

    /* Scratch float render buffer (avoids malloc on audio thread) */
    float  mix_buf[2048 * 2];
} Rg35Sf2Engine;

#ifdef RG35XXSP_SF2_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

static inline bool rg35_sf2_file_exists(const char *path) {
    return (path && path[0] && access(path, R_OK) == 0);
}

/*
 * Initialize SoundFont engine by searching standard RG35xxSP, Knulli, and Linux paths.
 * Returns true if a SoundFont was loaded; false if none was found (caller should fall back).
 */
static inline bool rg35_sf2_init(Rg35Sf2Engine *eng, int sample_rate, int max_voices,
                                 float reverb_wet, float master_gain) {
    if (!eng) return false;
    memset(eng, 0, sizeof(*eng));
    eng->sample_rate = (sample_rate > 0) ? sample_rate : 44100;
    eng->reverb_wet  = reverb_wet;
    eng->master_gain = (master_gain > 0.0f) ? master_gain : 1.35f;

    const char *env_sf2 = getenv("RG35XXSP_SOUNDFONT");
    const char *candidates[] = {
        env_sf2,
        "./GeneralUser-GS.sf2",
        "../soundfonts/GeneralUser-GS.sf2",
        "/userdata/roms/ports/soundfonts/GeneralUser-GS.sf2",
        "/userdata/roms/ports/pocketband/GeneralUser-GS.sf2",
        "./pocketband.sf2",
        "../soundfonts/pocketband.sf2",
        "/userdata/roms/ports/pocketband/pocketband.sf2",
        "/userdata/roms/ports/soundfonts/pocketband.sf2",
        "/usr/share/sounds/sf2/TimGM6mb.sf2",
        "/usr/share/sounds/sf2/default-GM.sf2",
        "/usr/share/soundfonts/default.sf2",
        NULL
    };

    for (int i = 0; candidates[i] != NULL || i == 0; i++) {
        if (i == 0 && !candidates[0]) continue;
        if (!candidates[i]) break;
        if (!rg35_sf2_file_exists(candidates[i])) continue;

        tsf *f = tsf_load_filename(candidates[i]);
        if (f) {
            tsf_set_output(f, TSF_STEREO_INTERLEAVED, eng->sample_rate, 0.0f);
            tsf_set_max_voices(f, (max_voices > 0) ? max_voices : 64);
            /* Initialize standard GM drum channel 9 as percussion bank 128 */
            tsf_channel_set_bank_preset(f, 9, 128, 0);
            eng->synth = f;
            snprintf(eng->loaded_path, sizeof(eng->loaded_path), "%s", candidates[i]);
            return true;
        }
    }
    return false;
}

static inline void rg35_sf2_free(Rg35Sf2Engine *eng) {
    if (!eng) return;
    if (eng->synth) {
        tsf_close(eng->synth);
        eng->synth = NULL;
    }
}

static inline bool rg35_sf2_ready(const Rg35Sf2Engine *eng) {
    return (eng && eng->synth != NULL);
}

static inline void rg35_sf2_set_channel_program(Rg35Sf2Engine *eng, int channel,
                                                int bank, int preset_num) {
    if (!eng || !eng->synth) return;
    int idx = tsf_get_presetindex(eng->synth, bank, preset_num);
    if (idx < 0 && bank == 128) idx = tsf_get_presetindex(eng->synth, 128, 0);
    if (idx < 0) idx = tsf_get_presetindex(eng->synth, 0, preset_num);
    if (idx < 0) idx = 0;
    tsf_channel_set_presetindex(eng->synth, channel, idx);
}

static inline void rg35_sf2_set_channel_pan_vol(Rg35Sf2Engine *eng, int channel,
                                                float pan_0to1, float vol_0to1) {
    if (!eng || !eng->synth) return;
    if (pan_0to1 < 0.0f) pan_0to1 = 0.0f;
    if (pan_0to1 > 1.0f) pan_0to1 = 1.0f;
    if (vol_0to1 < 0.0f) vol_0to1 = 0.0f;
    if (vol_0to1 > 1.0f) vol_0to1 = 1.0f;
    tsf_channel_set_pan(eng->synth, channel, pan_0to1);
    tsf_channel_set_volume(eng->synth, channel, vol_0to1);
}

static inline void rg35_sf2_note_on(Rg35Sf2Engine *eng, int channel, int key, float vel) {
    if (!eng || !eng->synth) return;
    if (key < 0) key = 0;
    if (key > 127) key = 127;
    if (vel < 0.0f) vel = 0.0f;
    if (vel > 1.0f) vel = 1.0f;
    tsf_channel_note_on(eng->synth, channel, key, vel);
}

static inline void rg35_sf2_note_off(Rg35Sf2Engine *eng, int channel, int key) {
    if (!eng || !eng->synth) return;
    if (key < 0 || key > 127) return;
    tsf_channel_note_off(eng->synth, channel, key);
}

static inline void rg35_sf2_channel_notes_off(Rg35Sf2Engine *eng, int channel) {
    if (!eng || !eng->synth) return;
    tsf_channel_note_off_all(eng->synth, channel);
}

static inline void rg35_sf2_all_notes_off(Rg35Sf2Engine *eng) {
    if (!eng || !eng->synth) return;
    tsf_note_off_all(eng->synth);
}

/* Process one stereo float sample through warm studio room reverb + DC block + soft limiter */
static inline void rg35_sf2_fx_sample(Rg35Sf2Engine *e, float in_l, float in_r,
                                      float *out_l, float *out_r) {
    float wet = e->reverb_wet;
    float l = in_l;
    float r = in_r;

    if (wet > 0.001f) {
        const float fb = 0.78f;
        const float damp = 0.28f;
        float mono_in = 0.5f * (in_l + in_r);

        /* Left 4 parallel low-pass comb filters */
        float y0l = e->c0l[e->i0l]; e->c0l_lp = y0l * (1.0f - damp) + e->c0l_lp * damp;
        e->c0l[e->i0l] = mono_in + e->c0l_lp * fb;
        if (++e->i0l >= SF2_REV_C0_L) e->i0l = 0;

        float y1l = e->c1l[e->i1l]; e->c1l_lp = y1l * (1.0f - damp) + e->c1l_lp * damp;
        e->c1l[e->i1l] = mono_in + e->c1l_lp * fb;
        if (++e->i1l >= SF2_REV_C1_L) e->i1l = 0;

        float y2l = e->c2l[e->i2l]; e->c2l_lp = y2l * (1.0f - damp) + e->c2l_lp * damp;
        e->c2l[e->i2l] = mono_in + e->c2l_lp * fb;
        if (++e->i2l >= SF2_REV_C2_L) e->i2l = 0;

        float y3l = e->c3l[e->i3l]; e->c3l_lp = y3l * (1.0f - damp) + e->c3l_lp * damp;
        e->c3l[e->i3l] = mono_in + e->c3l_lp * fb;
        if (++e->i3l >= SF2_REV_C3_L) e->i3l = 0;

        /* Right 4 parallel low-pass comb filters (slightly offset lengths for wide stereo image) */
        float y0r = e->c0r[e->i0r]; e->c0r_lp = y0r * (1.0f - damp) + e->c0r_lp * damp;
        e->c0r[e->i0r] = mono_in + e->c0r_lp * fb;
        if (++e->i0r >= SF2_REV_C0_R) e->i0r = 0;

        float y1r = e->c1r[e->i1r]; e->c1r_lp = y1r * (1.0f - damp) + e->c1r_lp * damp;
        e->c1r[e->i1r] = mono_in + e->c1r_lp * fb;
        if (++e->i1r >= SF2_REV_C1_R) e->i1r = 0;

        float y2r = e->c2r[e->i2r]; e->c2r_lp = y2r * (1.0f - damp) + e->c2r_lp * damp;
        e->c2r[e->i2r] = mono_in + e->c2r_lp * fb;
        if (++e->i2r >= SF2_REV_C2_R) e->i2r = 0;

        float y3r = e->c3r[e->i3r]; e->c3r_lp = y3r * (1.0f - damp) + e->c3r_lp * damp;
        e->c3r[e->i3r] = mono_in + e->c3r_lp * fb;
        if (++e->i3r >= SF2_REV_C3_R) e->i3r = 0;

        float rev_l = 0.25f * (y0l + y1l + y2l + y3l);
        float rev_r = 0.25f * (y0r + y1r + y2r + y3r);

        /* Two cascaded stereo all-pass diffusers */
        float b0l = e->ap0l[e->iap0];
        float o0l = -rev_l + b0l;
        e->ap0l[e->iap0] = rev_l + b0l * 0.5f;
        float b0r = e->ap0r[e->iap0];
        float o0r = -rev_r + b0r;
        e->ap0r[e->iap0] = rev_r + b0r * 0.5f;
        if (++e->iap0 >= SF2_REV_AP0) e->iap0 = 0;

        float b1l = e->ap1l[e->iap1];
        float o1l = -o0l + b1l;
        e->ap1l[e->iap1] = o0l + b1l * 0.5f;
        float b1r = e->ap1r[e->iap1];
        float o1r = -o0r + b1r;
        e->ap1r[e->iap1] = o0r + b1r * 0.5f;
        if (++e->iap1 >= SF2_REV_AP1) e->iap1 = 0;

        l += o1l * wet;
        r += o1r * wet;
    }

    /* 20 Hz stereo DC-blocking filter */
    float dl = l - e->dc_xl + 0.997f * e->dc_yl;
    e->dc_xl = l; e->dc_yl = dl;
    float dr = r - e->dc_xr + 0.997f * e->dc_yr;
    e->dc_xr = r; e->dc_yr = dr;

    /* Warm soft-clip mastering limiter */
    *out_l = tanhf(dl * e->master_gain);
    *out_r = tanhf(dr * e->master_gain);
}

/*
 * Render `frames` stereo frames from SoundFont + room reverb into float stereo interleaved
 * buffer (adding if `add_to_buf` is true, or overwriting if false).
 */
static inline void rg35_sf2_render_float(Rg35Sf2Engine *eng, float *dst_stereo, int frames,
                                         bool add_to_buf) {
    if (!eng || !eng->synth || !dst_stereo || frames <= 0) return;
    int offset = 0;
    while (offset < frames) {
        int chunk = frames - offset;
        if (chunk > 2048) chunk = 2048;
        tsf_render_float(eng->synth, eng->mix_buf, chunk, 0);
        for (int i = 0; i < chunk; i++) {
            float ol, or_;
            rg35_sf2_fx_sample(eng, eng->mix_buf[i * 2], eng->mix_buf[i * 2 + 1], &ol, &or_);
            int idx = (offset + i) * 2;
            if (add_to_buf) {
                dst_stereo[idx]     += ol;
                dst_stereo[idx + 1] += or_;
            } else {
                dst_stereo[idx]     = ol;
                dst_stereo[idx + 1] = or_;
            }
        }
        offset += chunk;
    }
}

/*
 * Render `frames` stereo 16-bit signed PCM frames directly into `dst_i16`.
 */
static inline void rg35_sf2_render_i16(Rg35Sf2Engine *eng, int16_t *dst_i16, int frames) {
    if (!eng || !eng->synth || !dst_i16 || frames <= 0) return;
    int offset = 0;
    while (offset < frames) {
        int chunk = frames - offset;
        if (chunk > 2048) chunk = 2048;
        tsf_render_float(eng->synth, eng->mix_buf, chunk, 0);
        for (int i = 0; i < chunk; i++) {
            float ol, or_;
            rg35_sf2_fx_sample(eng, eng->mix_buf[i * 2], eng->mix_buf[i * 2 + 1], &ol, &or_);
            int sl = (int)(ol * 31500.0f);
            int sr = (int)(or_ * 31500.0f);
            if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
            if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
            dst_i16[(offset + i) * 2]     = (int16_t)sl;
            dst_i16[(offset + i) * 2 + 1] = (int16_t)sr;
        }
        offset += chunk;
    }
}

#endif /* RG35XXSP_SF2_IMPLEMENTATION */
#endif /* RG35XXSP_SF2_H */
