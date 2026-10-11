// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// ============================================================================
// rs_student_fx: the out-of-tree effects component (THE course sandbox)
// ============================================================================
// The firmware core ships only bypass and the channel delay. Your effects
// live in this component and register here. Channel is registered the same
// way: main/audio/channel_fx.c is laid out like an effect file here, and the
// core's dsp_effects_init() calls its init and dsp_effects_register(), as
// rs_user_effects_init() does below.
//
// To add your own effect, write it as a new file in this directory (for
// example myeffect_fx.c), then make four edits (docs/ADDING_EFFECTS.md,
// "Registering your effect in the sandbox"):
//   1. This component's CMakeLists.txt: add the file to SRCS and to the -O2
//      set_source_files_properties() list.
//   2. Your file: make the record non-static,
//      const dsp_effect_t g_fx_myeffect = { ... };
//   3. student_fx.h: declare extern const dsp_effect_t g_fx_myeffect; and,
//      if the effect allocates at boot, void myeffect_init(void);
//   4. rs_user_effects_init() below, at the marked line: call your init (if
//      any) and dsp_effects_register(&g_fx_myeffect);
// The CLI `mode` list, the web UI's Mode buttons, `status`, the LEDs, the
// front-panel Feature cycle, and `settings save` all pick it up
// automatically. See docs/ADDING_EFFECTS.md for the full walkthrough (hooks,
// thread-safety rules, CPU budget).
//
// To prove nothing in the core depends on this component: delete this
// directory and rebuild. The firmware still runs (bypass + channel only).

#include "student_fx.h"

// The firmware declares rs_user_effects_init() weak and calls it at the end
// of dsp_effects_init(), after the boot geometry is final (so per-pair
// allocations here see the real pair count) and before settings_store_init
// applies a saved default (so a persisted effect resolves by name at boot).
// This definition overrides the weak no-op at link time (the component's
// CMakeLists uses WHOLE_ARCHIVE to guarantee it is linked).
void rs_user_effects_init(void)
{
    // myeffect_init();                          // only if your effect has an init
    // dsp_effects_register(&g_fx_myeffect);     // <- your effect goes here
}
