// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/sdmmc_host.h"

// Pin definitions and HAL API come from the board-specific header,
// selected by CONFIG_BOARD_REVISION in menuconfig.
#include "hal.h"

// ==========================================================
// Audio Pipeline Constants
// ==========================================================
#define FS_HZ                  48000            // Sample rate (Hz)
// Std-stereo baseline format + ping-pong CAPACITY terms. The LIVE slot config
// is g_geom (see "Runtime audio geometry" below) — TDM runs 16-bit slots and
// up to 6 RX channels; these constants size the buffers and describe the
// std path only.
#define AUDIO_BITS_PER_SAMPLE  32
#define AUDIO_CHANNELS         2
#define AUDIO_BYTES_PER_SAMPLE (AUDIO_BITS_PER_SAMPLE / 8)
#define AUDIO_BYTE_RATE        (FS_HZ * AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE)  // 384000 B/s

#define PP_FRAMES           512                 // Max frames per block (std-stereo value; runtime count is g_geom.pp_frames)
#define PP_BYTES            (PP_FRAMES * AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE) // ping-pong buffer CAPACITY (4096 B)

// Hardware ceiling on codec pairs (main + 2 expansions — the S3's 128-bit TDM
// frame cap, see the geometry note below). Sizes the per-pair DSP state and
// mix-gain tables so a fourth pair can never be half-plumbed.
#define AUDIO_MAX_PAIRS     3

// ==========================================================
// Runtime audio geometry
// ==========================================================
// The block BYTE size is capped at PP_BYTES (4096) in every mode so the four
// static internal ping-pong buffers never grow — the internal-RAM ledger is
// unchanged by TDM. What varies at runtime is how those bytes divide into
// frames × channels × slot width:
//   std stereo (no expansion):  512 frames × 2 ch × 32-bit = 4096 B
//   TDM 4×32   (1 expansion):   256 frames × 4 ch × 32-bit = 4096 B
//   TDM 8×16   (2 expansions;   256 frames × 6 ch × 16-bit = 3072 B (RX)
//              or forced 16)    256 frames × 4 ch × 16-bit = 2048 B (RX)
// TX always carries the stereo mixdown (slots 0-1): pp_frames × 2 × width.
// The ESP32-S3 TDM frame is hard-capped at 128 BCLK bits (I2S_LL_SLOT_FRAME_
// BIT_MAX), so 3 codecs REQUIRE 16-bit slots; 32-bit TDM tops out at 2 codecs.
// Computed once in app_main (from codec_probe_all + slot-width setting) BEFORE
// I2S init; read-only afterwards.
typedef struct {
    int    n_codecs;        // 1..3 (main + detected expansion boards)
    int    rx_ch;           // 2 * n_codecs
    int    slot_bits;       // 32 or 16 (memory sample width == I2S slot width)
    int    pp_frames;       // frames per block (512 std, 256 tdm)
    size_t rx_block_bytes;  // pp_frames * rx_ch * slot_bits/8   (≤ PP_BYTES)
    size_t tx_block_bytes;  // pp_frames * 2     * slot_bits/8   (≤ PP_BYTES)
    bool   tdm;             // false = std I2S stereo (today's path)
} audio_geom_t;
extern audio_geom_t g_geom;   // defined in radiosonic_main.c

// Recording/playback duration from a raw PCM byte count (48 kHz / stereo / 32-bit).
static inline float audio_duration_from_bytes(uint32_t bytes)
{
    return (float)bytes / (float)AUDIO_BYTE_RATE;
}

// ==========================================================
// Global State & Types
// ==========================================================
// DSP effects are a data-driven registry (see dsp_effects.c). The active effect
// is an index into the unified effect registry (app_state_t.dsp_effect);
// dsp_set() / dsp_current() rather than hard-coded mode constants.

typedef enum {
    INPUT_MIC = 0,
    // 1 was INPUT_LINE_SE (single-ended line in), removed 2026-09-27: from Rev 3
    // on, the line jack's sleeve is a sense line into the codec (tip/ring are
    // each measured against it), so SE routing read the sleeve on one channel
    // and a noise-prone leg on the other. The value stays reserved so saved
    // settings decode unchanged; a saved 1 loads as INPUT_LINE_DIFF.
    INPUT_LINE_DIFF = 2,   // line in: tip and ring, each against the jack sleeve
} input_mode_t;

// Effect configuration deliberately does NOT live here: each effect owns its
// own config privately (staged under dsp_cfg_lock) and exposes it only
// through its dsp_effect_t hooks — see dsp_effects.h. The app state carries
// only what the CORE owns.
typedef struct {
    input_mode_t  input_mode;
    int           input_gain_db;    // MICPGA, 0..+47 dB
    int           output_gain_db;   // HP/LO driver gain, -6..+29 dB
    int           dsp_effect;  // index into the effect registry (dsp_effect_at)
    sdmmc_card_t *sd_card;
} app_state_t;

// The pipeline control API (suspend/resume, heap_checkpoint, rx_overruns,
// debug buffer access) lives in audio_pipeline.h — this header is only the
// shared-state data contract.

// The one shared application state instance (defined in radiosonic_main.c).
// Writers follow the locking rules in dsp_effects.h (dsp_cfg_lock for
// multi-field configs; single aligned scalars are atomic).
extern app_state_t g_state;