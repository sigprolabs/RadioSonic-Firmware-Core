// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
// Internal wiring of the effects sandbox: each effect file exports its
// record and an optional init; student_fx.c registers them from
// rs_user_effects_init(). Nothing outside this component includes this
// header.
#include "dsp_effects.h"

// myeffect_fx.c: declare each effect's record, and its init if it has one.
// extern const dsp_effect_t g_fx_myeffect;
// void myeffect_init(void);
