// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
// The channel effect (channel_fx.c), wired like a sandbox effect: the file
// exports its record and an init, and dsp_effects_init() registers it
// with "channel_fx_init(); dsp_effects_register(&g_fx_channel);" before
// rs_user_effects_init() runs. components/rs_student_fx/student_fx.h is the
// same header for the sandbox's effects.
#include "dsp_effects.h"

// channel_fx.c: L/R multi-tap delay (the in-tree worked example)
extern const dsp_effect_t g_fx_channel;
void channel_fx_init(void);
