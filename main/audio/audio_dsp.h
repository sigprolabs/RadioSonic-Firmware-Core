// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdint.h>
#include <stddef.h>

// Allocate PSRAM buffers. Call this once in app_main.
// (Per-effect APIs live behind the dsp_effect_t hooks — dsp_effects.h.)
void dsp_init(void);

// Flush every effect's state (prevents playback audio bleeding into delay
// lines when live capture resumes).
void dsp_flush_state(void);

// Main processing loop. Buffers are raw block storage: stereo int32 in std
// mode; in TDM mode `in` is rx_ch interleaved channels and `out` the stereo
// mixdown, both at g_geom.slot_bits (int16-packed when 16).
void dsp_process(const int32_t *in, int32_t *out, size_t frames);

// TDM mixdown gains: contribution of each processed codec pair (0 = main) to
// the stereo output. Default 1.0 each; the sum is hard-clipped.
void  dsp_set_mix_gain(int pair, float gain);
float dsp_get_mix_gain(int pair);

// Per-block processing-time stats (µs), measured in dsp_process on live audio.
// avg/max are since the previous call (reading resets the accumulators).
// Block budget at 48 kHz is PP_FRAMES/FS_HZ = 10667 µs.
void dsp_get_proc_stats(uint32_t *last_us, uint32_t *avg_us, uint32_t *max_us);