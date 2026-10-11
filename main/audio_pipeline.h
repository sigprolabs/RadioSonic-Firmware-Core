// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// ==========================================================
// Audio pipeline control API — implemented in radiosonic_main.c
// ==========================================================
// radiosonic_main.c owns the audio tasks, queues, ping-pong buffers, and I2S
// handles; this header is the surface other modules use to talk to it.
// (Split out of common.h so the shared-state header stays a pure data
// contract and the include graph shows who actually depends on the pipeline.)

// Suspend/resume for light sleep.
// Ordering: suspend → codec_sleep already done → sleep →
//           clocks_on (MCLK back for codec PLL) → codec_wake → resume.
esp_err_t audio_pipeline_suspend(void);   // park rx/dsp/tx/led tasks, disable I2S
void      audio_pipeline_clocks_on(void); // re-enable I2S channels (MCLK to codec)
esp_err_t audio_pipeline_resume(void);    // flush DSP state, unpark tasks

// Internal-RAM watermark log line (tag "heap"), called after each boot phase and
// from httpd start/retry. The budget rule lives in docs/ARCHITECTURE.md
// ("Internal RAM budget"): the largest free block at the wifi-up checkpoint must
// stay above the biggest late allocation (currently httpd's task stack,
// HTTPD_STACK_SIZE).
void heap_checkpoint(const char *phase);

// Lifetime count of I2S RX DMA queue overruns (audio lost before rx_fill could
// read it — e.g. both cores stalled by a flash-cache suspension, issue #12).
// Loss upstream of every consumer: recordings splice, and no ring-level drop
// counter can see it. Consumers snapshot at start and diff at stop.
uint32_t audio_pipeline_rx_overruns(void);

// Raw main-codec input, channels 0/1, over the last ~0.34 s window: mean (the
// ADC's DC offset — the inputs are AC-coupled), AC rms and peak |sample|, all
// in int32 sample units (full scale 2^31). seq increments per window, so an
// unchanged seq means the pipeline is parked (sleep) or not started. False in
// TDM 8x16 (not monitored). Consumer: the stuck-input guard (input_guard.c).
bool audio_pipeline_input_stats(int32_t mean[2], int32_t rms[2], int32_t peak[2], uint32_t *seq);

// Debug view of the most recently processed audio block (CLI `dump` command).
// Returns a pointer to the live RX or TX ping-pong buffer — data races with
// the running pipeline by design; fine for an eyeball/CSV dump, meaningless
// for sample-exact analysis. frames_out (optional) receives g_geom.pp_frames.
const int32_t *audio_pipeline_debug_buf(bool rx, int *frames_out);
