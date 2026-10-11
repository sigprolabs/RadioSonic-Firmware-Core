// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Codec instances on the shared SPI control bus. CODEC_MAIN is the on-board
// AIC3204; CODEC_EXP1/EXP2 are the optional Microphone Expansion boards
// (AIC3204 + stereo mics) on CODEC_1_CS/CODEC_2_CS. Every board rev routes the
// expansion connector; presence is probed at boot (codec_probe_all) and drives
// the I2S geometry choice (std stereo vs TDM) in app_main.
typedef enum {
    CODEC_MAIN = 0,
    CODEC_EXP1,
    CODEC_EXP2,
    CODEC_COUNT
} codec_id_t;

// AIC3204 gain ranges — hardware constraints with ONE owner. Every UI's range
// check and clamp (CLI, web, buttons, pot, settings load) uses these; a codec
// change lands here, not in six files of literals.
#define CODEC_INPUT_GAIN_MIN_DB     0    // MICPGA
#define CODEC_INPUT_GAIN_MAX_DB    47
#define CODEC_OUTPUT_GAIN_MIN_DB  (-6)   // HP/LO driver stages
#define CODEC_OUTPUT_GAIN_MAX_DB   29
// DAC digital volume, in 0.5 dB units. The register reaches +24 dB, but
// anything above 0 dB clips a full-scale signal, so 0 dB is the ceiling.
#define CODEC_DAC_VOL_MIN        (-127)  // -63.5 dB
#define CODEC_DAC_VOL_MAX           0
// Input-select policy defaults (applied when the source is switched).
#define CODEC_DEFAULT_MIC_GAIN_DB  20
#define CODEC_DEFAULT_LINE_GAIN_DB  0

// Initialize the SPI device handles (main + expansion CS lines)
void codec_init_spi(void);

// Probe the expansion CS lines for attached codecs (page-select write/readback,
// two distinct values — MISO's boot pullup makes an absent board read 0xFF).
// Needs only SPI, not MCLK; call after codec_init_spi(), before I2S geometry
// is chosen. Returns the total number of codecs present (1..3).
int  codec_probe_all(void);
// Diagnostic re-probe exposing raw readback bytes (CLI `codec probe`);
// never changes the boot presence verdict or geometry. The _at variant
// repeats it through a temporary device at `hz` on the same CS and reports
// the clock actually generated — sweep it to check SPI read integrity
// (measured 2026-07-27: reads are clean 1-20 MHz on a healthy bus; failures
// indicate wiring/loading, not timing — see audio_codec.c).
bool codec_probe_raw(codec_id_t id, uint8_t *v1, uint8_t *v2);
bool codec_probe_raw_at(codec_id_t id, int hz, int *actual_khz,
                        uint8_t *v1, uint8_t *v2);
// Boot-time main-codec probe result, for truthful status reporting only —
// the main codec is never marked absent (see codec_probe_all).
bool codec_main_probe_ok(void);
// Detach/re-attach all codec SPI devices. codec_probe_raw_at() restores its
// own routing; call this after codec_cs_park(), which deliberately leaves the
// CS pin parked (the temporary-device CS steal never returns on its own).
void codec_spi_reattach(void);
// CS pin number for a codec instance (false if absent on this revision).
bool codec_cs_pin(codec_id_t id, int *pin);
// Park a CS pin at a static level for DMM continuity checks through J5.
// Breaks SPI for that codec until codec_spi_reattach().
bool codec_cs_park(codec_id_t id, int level);
bool codec_present(codec_id_t id);
int  codec_count(void);          // 1 + number of expansion codecs found

// Initialize every present codec for the chosen geometry (from g_geom).
// tdm=false is the legacy single-codec std-I2S path. MCLK must already be
// running (call after i2s_channel_enable — the codec PLLs reference it).
void codec_init_all(bool tdm, int slot_bits);

// TDM bring-up measurement verbs (CLI `codec r63|offset`, lr_test protocol):
// live R63 data-path flip on the main codec, and live rewrite of every active
// codec's R28 data offset from a new base. Default base is 0 — MEASURED on
// hardware 2026-07-27; the pre-measurement guess of 1 was wrong (see
// s_tdm_base in audio_codec.c for the failure signature it produced).
void codec_dbg_r63(bool straight);
void codec_dbg_tdm_base(uint8_t base_bclks);

// Apply dynamic settings (Input routing, Gain) based on g_state
void codec_apply_settings(void);

// Apply g_state.output_gain_db (-6..+29 dB, 1 dB steps) to the HP and
// line-out driver stages. Also called by codec_apply_settings/codec_wake.
// Main codec only (expansion output drivers stay off).
void codec_apply_output_gain(void);

// DAC digital volume in 0.5 dB units, CODEC_DAC_VOL_MIN..MAX (clamped):
// the pot's volume target. Attenuates the outputs only (record taps are
// upstream of the DAC). Not saved; boots at 0 dB and survives codec re-init
// and sleep. Main codec only.
void codec_set_dac_volume(int half_db);
int  codec_get_dac_volume(void);

// Apply g_state.input_gain_db (0..47 dB) to the MICPGA only — use for
// gain-only changes (2 SPI writes vs the full routing rewrite). Applies to
// every active codec (expansion mic PGAs track the main input gain).
void codec_apply_input_gain(void);

// Light-sleep support: power down converters/drivers/PLL with all routing,
// volume, and interface registers retained (chip stays powered), and the
// matching restore. codec_sleep() needs I2S still clocking (clean mutes);
// codec_wake() needs MCLK already restored (PLL reference). Both fan out to
// every active codec.
void codec_sleep(void);
void codec_wake(void);

// Ramp DAC output from minimum (-63.5 dB) up to the set DAC volume
// (codec_set_dac_volume, 0 dB by default) over at most ~ramp_ms.
// codec_wake() leaves the volume at minimum; call this once the audio
// pipeline is running to fade live audio back in smoothly. Main codec only.
void codec_fade_in(uint32_t ramp_ms);

// codec_wake() leaves the ADC muted so the MICBIAS power-on transient can't
// reach the DSP. Call this after a settle delay (a few hundred ms), before
// codec_fade_in(). Fans out to every active codec.
void codec_unmute_adc(void);

// --- Readback diagnostics (CLI `codec rd|dump|check`, `status`) ---
// Every register write is atomic (page select + write under one lock) and
// shadowed, so the firmware can compare what it wrote with what the codec
// actually holds. Reading P0 R42/R44/R45 clears those sticky flags.
#include "esp_err.h"
esp_err_t codec_read_reg(codec_id_t id, uint8_t page, uint8_t reg, uint8_t *val);
typedef void (*codec_mismatch_fn)(uint8_t page, uint8_t reg, uint8_t wrote,
                                  uint8_t read, void *ctx);
// Reads back every shadowed register of `id`; calls report() per mismatch.
// Returns the number of registers checked (-1 on SPI failure).
int codec_check_shadow(codec_id_t id, codec_mismatch_fn report, void *ctx, int *mismatches);
uint32_t codec_spi_error_count(void);   // failed SPI transactions since boot
bool codec_is_active(codec_id_t id);
// Recovery rungs (they localise a fault by which one clears it):
void codec_adc_cycle(void);   // ADC power off 50 ms -> on; routing untouched
void codec_reinit(void);      // soft reset + full init + re-apply g_state
// Dead-LINE-IN recovery: ADC off, un-route, weakly drive every input to common
// mode for precharge_ms (0 = the current default), re-route, ADC on — for
// anything that knocked already-routed pins off common mode. Blocks for the
// precharge. codec_apply_settings runs the same sequence on an input change,
// waiting only for charge the new pins still lack (see audio_codec.c,
// "Input routing"). codec_recover_inputs_if is the guard's entry: it does
// nothing (returns false) if codec_input_epoch() has moved on from
// expected_epoch or the codec is asleep. codec_reroute_inputs is the old
// re-select rung: MICBIAS + routing + gains rewritten, no ADC cycle, no
// precharge (kept so the recovery ladder still localises faults).
void codec_recover_inputs(uint32_t precharge_ms);
bool codec_recover_inputs_if(uint32_t expected_epoch);
void codec_reroute_inputs(void);

// Precharge length used at boot, on wake and by `codec recover` (volatile;
// `codec precharge <ms>` for bench tuning).
void     codec_set_precharge_ms(uint32_t ms);
uint32_t codec_get_precharge_ms(void);

// For the input guard: epoch increments after every routing sequence, re-init
// and sleep (so the guard can let the inputs settle before judging them);
// busy is true while a sequence runs.
uint32_t codec_input_epoch(void);
bool     codec_input_routing_busy(void);

// Full-chip common mode 0.75 V (default, SLAA557 3.5 for AVDD < 1.8 V) or the
// pre-2026-09-28 0.9 V. Takes effect at the next init (`codec cm` re-inits).
void codec_set_cm_075(bool on);
bool codec_get_cm_075(void);

// Boot snapshot: pages 0/1 read BEFORE init. The codec stays powered across an
// MCU reset, so this preserves the pre-restart state (`codec snapshot`).
// Call after codec_init_spi(), before codec_probe_all().
void codec_snapshot_boot(void);
bool codec_boot_snapshot(uint8_t page, const uint8_t **regs);   // false if none
bool codec_boot_was_running(void);   // codec was configured before this boot
bool codec_was_adopted(void);        // diag build kept that state (no init)

// DIAGNOSTIC BUILDS ONLY (set to 1 for a bench build, never commit as 1):
// adopt a running codec at boot instead of soft-resetting it, and allow raw
// register writes (`codec wr`) — for trying recovery steps on a live fault.
#ifndef CODEC_DIAG_ADOPT
#define CODEC_DIAG_ADOPT 0
#endif
#if CODEC_DIAG_ADOPT
void codec_write_reg(codec_id_t id, uint8_t page, uint8_t reg, uint8_t val);
#endif
