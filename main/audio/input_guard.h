// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdbool.h>

// Stuck-input guard: watches the raw input's DC offset and level and runs
// codec_recover_inputs() when the codec's input stage looks stuck (the LINE
// IN "no input" fault). Detection rules and limits: input_guard.c.
void     input_guard_start(void);          // app_main, after the audio tasks
void     input_guard_enable(bool on);      // `codec guard on|off`; on also re-arms a stood-down guard
void     input_guard_test(void);           // `codec guard test`: run the recovery branch once (bench:
                                           //  exercises the deepest stack path for the high-water mark)
void     input_guard_print_status(void);   // one line for `status` / `codec guard`
unsigned input_guard_stack_free(void);     // task stack high-water mark, bytes (0 = not running)
unsigned input_guard_stack_size(void);
