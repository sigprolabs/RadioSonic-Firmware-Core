// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// ==========================================================
// DSP framework
// ==========================================================
// This file is the framework students can ignore: it converts the I2S int32
// (Q31) stream to/from float, does the straight textbook L/R unpack (see the
// measured note at the unpack loop — no swapping happens here), runs the
// per-sample loop, clips the output, and dispatches to the currently selected
// effect. The effects themselves (the math) live in channel_fx.c and
// components/rs_student_fx/.

#include "audio_dsp.h"
#include "dsp_effects.h"
#include "common.h"
#include "esp_timer.h"
#include <string.h>

// Q31 (int32 full-scale) <-> float [-1,+1] conversion.
#define Q31_TO_FLOAT    (1.0f / 2147483648.0f)   // 1 / 2^31
#define FLOAT_TO_Q31    2147483647.0f            // 2^31 - 1
// Q15 equivalents for the 16-bit TDM slot mode.
#define Q15_TO_FLOAT    (1.0f / 32768.0f)        // 1 / 2^15
#define FLOAT_TO_Q15    32767.0f                 // 2^15 - 1

// Per-block load meter (see dsp_get_proc_stats). TWO writers, deliberately
// unlocked: the DSP task accumulates, and the console's read-and-reset zeroes
// the counters — a reset racing an in-flight block can drop that one block
// from the stats. Acceptable for a load meter; noted because the repo
// otherwise teaches strict single-writer volatile publishing.
static volatile uint32_t s_proc_last_us = 0;
static volatile uint32_t s_proc_max_us  = 0;
static volatile uint32_t s_proc_sum_us  = 0;
static volatile uint32_t s_proc_blocks  = 0;

void dsp_get_proc_stats(uint32_t *last_us, uint32_t *avg_us, uint32_t *max_us)
{
    uint32_t n = s_proc_blocks;
    if (last_us) *last_us = s_proc_last_us;
    if (avg_us)  *avg_us  = n ? s_proc_sum_us / n : 0;
    if (max_us)  *max_us  = s_proc_max_us;
    s_proc_sum_us = 0;
    s_proc_blocks = 0;
    s_proc_max_us = 0;
}

void dsp_init(void)
{
    dsp_effects_init();
}

// Per-pair mixdown gains (TDM): how much of each processed codec pair lands
// in the stereo output. Aligned-float writes are atomic on Xtensa, so the
// setter needs no lock against the DSP task's reads.
static float s_mix_gain[AUDIO_MAX_PAIRS] = { 1.0f, 1.0f, 1.0f };

void dsp_set_mix_gain(int pair, float gain)
{
    if (pair < 0 || pair >= AUDIO_MAX_PAIRS) return;
    s_mix_gain[pair] = gain;
}

float dsp_get_mix_gain(int pair)
{
    return (pair >= 0 && pair < AUDIO_MAX_PAIRS) ? s_mix_gain[pair] : 0.0f;
}

// Call whenever the audio source switches (e.g. playback ends, returning to
// live capture) so stale samples in the effect delay lines don't bleed in.
void dsp_flush_state(void)
{
    dsp_effects_reset_all();
}

// Per-block real-time entry point. Framework concerns only — the active effect
// supplies the math, one stereo sample at a time.
void dsp_process(const int32_t *in, int32_t *out, size_t frames)
{
    const int64_t t0 = esp_timer_get_time();
    const float scale   = Q31_TO_FLOAT;
    const float rescale = FLOAT_TO_Q31;

    // Snapshot the active effect and its config once per block: a mode or
    // coefficient change mid-block takes effect on the next block, and every
    // sample in this block sees one coherent set of coefficients.
    const dsp_effect_t *fx = dsp_current();
    dsp_effects_snapshot();

    if (!g_geom.tdm) {
        // Textbook Philips unpack: first word of each frame (WS-low slot) = LEFT.
        // Verified by measurement 2026-07-08 (issue #1): the S3's I2S RX really is
        // textbook, so xL/xR here are the physical left/right. The chain's single
        // digital inversion is on the TX side and is compensated at the codec
        // (R63 cross-map, audio_codec.c) — keep this unpack straight.
        for (size_t n = 0; n < frames; n++) {
            float xL = (float)in[2*n]   * scale;
            float xR = (float)in[2*n+1] * scale;

            float yL, yR;
            fx->process(xL, xR, &yL, &yR);

            // Hard clip to [-1, +1]
            if (yL >  1.0f) yL =  1.0f;
            if (yL < -1.0f) yL = -1.0f;
            if (yR >  1.0f) yR =  1.0f;
            if (yR < -1.0f) yR = -1.0f;

            out[2*n]   = (int32_t)(yL * rescale);
            out[2*n+1] = (int32_t)(yR * rescale);
        }
    } else {
        // TDM per-channel DSP: RX frames carry rx_ch packed channels (one
        // stereo pair per codec, pair 0 = main). Each pair runs the SAME
        // selected effect on its OWN state (dsp_effects_select_pair), and the
        // processed pairs sum into a stereo mix for the main-codec DAC (TX
        // slots 0-1). Pair-outer / sample-inner keeps each pair's delay lines
        // hot in cache. The hard clip after summing is the saturation stage.
        const int ch    = g_geom.rx_ch;
        const int pairs = ch / 2;
        // The buffers are raw DMA block storage (see the dsp_process contract
        // in audio_dsp.h); in 16-bit slot mode the same bytes are int16-packed,
        // so this reinterpreting cast over raw memory is deliberate.
        const int16_t *in16 = (const int16_t *)in;   // used when slot_bits==16

        // Mix accumulator: TDM blocks are ≤ 256 frames (PP_BYTES/16 at 8ch×32
        // or 2048/8 — see g_geom), so this is 2 KB of .bss — under the 4 KB
        // internal-RAM ledger threshold.
        static float mix[PP_BYTES / 16][2];
        const size_t nf = (frames > PP_BYTES / 16) ? PP_BYTES / 16 : frames;
        memset(mix, 0, nf * sizeof(mix[0]));

        for (int p = 0; p < pairs; p++) {
            if (!dsp_effects_select_pair(p)) continue;  // absent/failed pair
            const float g = dsp_get_mix_gain(p);
            for (size_t n = 0; n < nf; n++) {
                float xL, xR;
                if (g_geom.slot_bits == 32) {
                    xL = (float)in[n*ch + 2*p]     * scale;
                    xR = (float)in[n*ch + 2*p + 1] * scale;
                } else {
                    xL = (float)in16[n*ch + 2*p]     * Q15_TO_FLOAT;
                    xR = (float)in16[n*ch + 2*p + 1] * Q15_TO_FLOAT;
                }
                float yL, yR;
                fx->process(xL, xR, &yL, &yR);
                mix[n][0] += g * yL;
                mix[n][1] += g * yR;
            }
        }
        dsp_effects_select_pair(0);

        // Clip + repack the stereo mixdown at the slot width.
        if (g_geom.slot_bits == 32) {
            for (size_t n = 0; n < nf; n++) {
                float yL = mix[n][0], yR = mix[n][1];
                if (yL >  1.0f) yL =  1.0f;
                if (yL < -1.0f) yL = -1.0f;
                if (yR >  1.0f) yR =  1.0f;
                if (yR < -1.0f) yR = -1.0f;
                out[2*n]   = (int32_t)(yL * rescale);
                out[2*n+1] = (int32_t)(yR * rescale);
            }
        } else {
            int16_t *o16 = (int16_t *)out;
            for (size_t n = 0; n < nf; n++) {
                float yL = mix[n][0], yR = mix[n][1];
                if (yL >  1.0f) yL =  1.0f;
                if (yL < -1.0f) yL = -1.0f;
                if (yR >  1.0f) yR =  1.0f;
                if (yR < -1.0f) yR = -1.0f;
                o16[2*n]   = (int16_t)(yL * FLOAT_TO_Q15);
                o16[2*n+1] = (int16_t)(yR * FLOAT_TO_Q15);
            }
        }
    }

    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    s_proc_last_us = dt;
    s_proc_sum_us += dt;
    s_proc_blocks++;
    if (dt > s_proc_max_us) s_proc_max_us = dt;
}
