// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

// Analog control pot (Rev 3 / Rev A: wiper on IO1 = ADC1_CH0, see hal_pot()).
//
// One knob, one assignable target — switched at runtime with the `pot` console
// command:
//   off     the knob is read but drives nothing (default at boot)
//   volume  codec DAC digital volume, -63.5 dB (CCW) .. 0 dB (CW), 0.5 dB
//           steps (codec_set_dac_volume). `gain out` (the HP/LO drivers,
//           -6..+29 dB) is a separate stage and sets the level at full
//           volume. The knob and the gain buttons write different registers.
//   fx      the CURRENT effect's pot binding (dsp_effect_t.pot_set — a knob
//           position 0..1 handed to whatever the effect exposes; the channel
//           effect maps it to a tap delay, `channel pot` tunes which/range).
//           Effects without a binding just ignore the knob.
//
// SOFT TAKEOVER: the pot only writes when its own filtered reading moves more
// than a dead-band. Selecting a target, or a value set elsewhere (`channel`, a
// `settings load`), therefore sticks until the knob is physically turned — the
// pot never stomps another control just because it is sitting somewhere.
// `pot apply` forces the knob's current position to take effect immediately.
// Caveat: this is movement-ARMING, not value-matching pickup — on takeover the
// parameter jumps straight to the knob's absolute position (up to the full
// range in one step, e.g. 0 dB to -63.5 dB of volume). True pickup is a
// possible future refinement.
//
// The knob position is scaled to the clockwise stop (2.87 V, set by R12), so
// full CW reads 100 % (issue #19).
//
// No task of its own: control_pot_poll() is called from the 20 ms ctrl_btn
// poll loop (control_buttons.c) so this costs no internal RAM for a stack and
// adds nothing to the core-0 priority ladder. A future board with a pot but no
// panel buttons would need its own poller — control_buttons_init() returns
// early (never creating the task) when hal_panel_buttons() is false.

typedef enum {
    POT_TARGET_OFF = 0,
    POT_TARGET_VOLUME,   // codec DAC digital volume (codec_set_dac_volume)
    POT_TARGET_FX,       // current effect's pot_set hook (dsp_effects.h)
} pot_target_t;

// Claim the ADC1 unit + pot channel. No-op (ESP_OK) on boards without a pot.
// Call before control_buttons_init() so the first poll has a configured channel.
esp_err_t control_pot_init(void);

// Point the knob at a target. Re-arms soft takeover: the pot stays quiet until
// it is physically moved past the dead-band.
void control_pot_set_target(pot_target_t t);

// Sample + act. Called from the ctrl_btn task every BTN_POLL_MS; no-op if the
// board has no pot or init failed.
void control_pot_poll(void);

// Register the `pot` console command.
void register_pot_cmd(void);

// One-line summary for `status` (prints nothing on boards without a pot).
void control_pot_print_status(void);

// The shared ADC1 oneshot handle, or NULL if the pot is not initialised.
// IDF allows only ONE oneshot unit handle per ADC unit, so the `adc` CLI test
// command must borrow this one for ADC1 pins instead of creating its own.
adc_oneshot_unit_handle_t control_pot_adc_unit(void);
