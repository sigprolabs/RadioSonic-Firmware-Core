// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once

// Status-LED and meter-needle POLICY (what the strip shows); the strip DRIVER
// (rgb_leds.c) only moves pixels. Split so the board layer stays free of app
// state: this module reads g_state + the DSP registry and paints; boards
// without a strip compile a self-stubbed driver and all of this no-ops.

// Render the current status onto the 4-LED strip. Cheap to poll (repaints only
// on state change); led_task calls it every 100 ms. Exception: while an effect
// publishes the framework meter (dsp_meter_publish in dsp_effects.h) it
// repaints every poll, rendering the live needle.
void led_status_render(void);

// Force a full repaint on the next render — call after anything that blanked
// or overrode the strip outside this module (e.g. rgb_leds_all_off before
// sleep).
void led_status_invalidate(void);
