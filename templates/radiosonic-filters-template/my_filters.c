// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// RadioSonic effect component: starting skeleton.
//
// As shipped it registers no effect, so the firmware runs with the two
// effects in its core, bypass and channel. To add yours, uncomment the block
// below, rename "myeffect", write the math in fx_myeffect(), and uncomment
// the lines in rs_user_effects_init() at the bottom.
//
// The worked example is the channel effect in main/audio/channel_fx.c. It
// sets every hook except alias (the fx_channel* functions), allocates its
// per-pair state in channel_fx_init(), and exports its record, g_fx_channel.
// main/audio/channel_fx.h declares the record and the init function, and the
// core's dsp_effects_init() registers channel with
//     channel_fx_init();
//     dsp_effects_register(&g_fx_channel);
// the same two calls rs_user_effects_init() below makes for your effect.
// Channel's web card is main/audio/channel_panel.html. docs/ADDING_EFFECTS.md
// has the full contract: float ranges, the 48 kHz real-time budget, thread
// safety and per-pair state.
#include "dsp_effects.h"    // dsp_effect_t, dsp_effects_register, dsp_cfg_lock, dsp_current_pair
#include "common.h"         // FS_HZ, AUDIO_MAX_PAIRS
// The headers channel_fx.c includes for its hooks; keep the ones you use.
#include <math.h>           // isnan, to ignore NaN in your config writer
#include <stdio.h>          // printf, snprintf, sscanf
#include <stdlib.h>         // strtol, strtod
#include <string.h>         // memset, strcmp, strchr
#include "esp_heap_caps.h"  // heap_caps_malloc
#include "esp_log.h"        // ESP_LOGE, with your own static const char *TAG

// static void fx_myeffect(float xL, float xR, float *yL, float *yR)
// {
//     // One stereo sample in and out, as floats in [-1, +1]. This runs once
//     // per sample (48 kHz) for each codec pair on the audio core, so it
//     // must not printf, allocate memory or block.
//     *yL = xL;
//     *yR = xR;
// }
//
// // Optional: allocate per-pair state at boot, as channel_fx_init() does.
// // dsp_effect_t has no init field; rs_user_effects_init() calls this by name.
// void myeffect_init(void)
// {
// }
//
// // The registry keeps a pointer to this record, so define it at file scope,
// // where it exists for the whole run, never as a local inside a function.
// // Like g_fx_channel it has no static keyword, so another source file can
// // declare it extern and register it.
// const dsp_effect_t g_fx_myeffect = {
//     .name    = "myeffect",   // CLI and web key: lowercase, at most 15 chars
//     .label   = "My Effect",  // required: what `status` and `help` print
//     .group   = NULL,         // front-panel family; NULL = a group of its own
//     .led_rgb = 0x000000,     // Mode LED color as 0xRRGGBB; 0 shows dim white
//     .process = fx_myeffect,  // required
//     // Optional hooks; channel's version of each is in main/audio/channel_fx.c
//     // (it has no alias):
//     // .reset     = ...,     // fx_channel_reset
//     // .print     = ...,     // fx_channel_print
//     // .snapshot  = ...,     // fx_channel_snapshot
//     // .cmd       = ...,     // fx_channel_cmd
//     // .save_text = ...,     // fx_channel_save_text
//     // .load_text = ...,     // fx_channel_load_text
//     // .pot_label = "...",   // "channel delay"
//     // .pot_set   = ...,     // fx_channel_pot_set
//     // .web_panel = ...,     // your own fragment (channel's: _binary_channel_panel_html_start)
//     // .alias     = "...",   // a former name, still accepted after a rename
// };

// The firmware has a weak, empty default of this hook and calls it once at
// boot, at the end of DSP init, right after it registers channel. Only one
// component in the build may define it (see README.md).
void rs_user_effects_init(void)
{
    // myeffect_init();                        // only if your effect has an init
    // dsp_effects_register(&g_fx_myeffect);   // 8 slots: DSP_USER_EFFECT_SLOTS (9) less channel's
}
