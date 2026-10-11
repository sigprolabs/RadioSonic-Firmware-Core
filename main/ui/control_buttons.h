// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include "esp_err.h"

// Front-panel control buttons (Rev 2/3; no-op on Rev 1).
//
// The BOOT button is a shift (hold) modifier giving each top-row button two
// functions. The authoritative assignment table (positions btn1..btn5, both
// revs' silkscreen names, normal + shifted actions, and the long-hold
// behaviors: btn1 sleep, btn5 save-as-default) lives at the top of
// control_buttons.c — one table, kept next to the code that implements it.
//
// The feature/sub-mode structure is derived from the DSP effect registry at
// init, so a newly registered effect joins automatically.
//
// All actions write the same g_state fields the CLI commands use, so the
// `status` command always reflects the current button-selected state.
//
// Spawns a low-priority polling task pinned to Core 0 (its 20 ms loop also
// polls the analog pot — see control_pot.h).
esp_err_t control_buttons_init(void);

// Re-point the feature/sub-mode cursor at g_state.dsp_effect. Normally NOT
// needed by other modules: the button task re-syncs itself lazily whenever it
// notices the effect changed under it (CLI `mode`, web, settings load).
// Exposed for the init path and any future caller that wants the cursor
// aligned immediately. Safe any time; no-op before init or on Rev 1.
void control_buttons_sync(void);
