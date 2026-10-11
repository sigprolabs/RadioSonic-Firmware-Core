// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// ==========================================================
// DSP effect framework + bypass
// ==========================================================
// This file is the FRAMEWORK: the registry, the config-publish lock, the
// per-pair selection, the meter channel, and bypass, the one effect the
// registry itself defines (index 0, dsp_is_bypass). Every other effect lives
// in its own file and registers through dsp_effects_register(): the channel
// multi-tap delay in channel_fx.c, registered by dsp_effects_init() below,
// and any out-of-tree effects in components/rs_student_fx/, registered by
// rs_user_effects_init().
// No control surface (CLI, web, settings, LEDs, pot) has a private path to
// any effect; if a hook here can't express something, that is a framework
// limitation, not a reason to special-case an effect.
//
//   HOW TO ADD AN EFFECT: docs/ADDING_EFFECTS.md (full walkthrough).
//   channel_fx.c is the in-tree worked example of every hook except alias:
//   process/reset/print/snapshot/cmd/save_text/load_text/pot/web_panel.
//
// process() receives the input sample already scaled to float [-1,+1]; return
// the output in the same range (the framework clips and repacks). It runs
// 48000x/s on the audio core: pure math only — no printf/malloc/blocking.

#include "dsp_effects.h"
#include "audio_dsp.h"
#include "common.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "channel_fx.h"

static const char *TAG = "DSP";

// ----------------------------------------------------------
// Thread-safe config publish (framework)
// ----------------------------------------------------------
// Multi-field effect configs are written by the console / web / pot tasks and
// read by the DSP task. Writers stage under this short critical section; the
// effect's snapshot hook copies a coherent snapshot once per block under the
// same lock, so a block never mixes old and new parameters.
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

void dsp_cfg_lock(void)   { taskENTER_CRITICAL(&s_cfg_mux); }
void dsp_cfg_unlock(void) { taskEXIT_CRITICAL(&s_cfg_mux); }

// Per-block dispatch: only the CURRENT effect needs a coherent copy.
void dsp_effects_snapshot(void)
{
    const dsp_effect_t *fx = dsp_current();
    if (fx->snapshot) fx->snapshot();
}

// ----------------------------------------------------------
// Per-pair selection (framework)
// ----------------------------------------------------------
// TDM runs the sample loop once per codec pair (pair 0 = main codec). The
// framework selects the pair; effects that keep per-pair state index it with
// dsp_current_pair(). Pairs beyond this boot's geometry report false and the
// framework skips them (dsp_effects_init() enables the rest).
int         g_dsp_cur_pair = 0;   // read through dsp_current_pair() (inline, dsp_effects.h)
static bool s_pair_ok[AUDIO_MAX_PAIRS] = { true, false, false };

bool dsp_effects_select_pair(int p)
{
    if (p < 0 || p >= AUDIO_MAX_PAIRS || !s_pair_ok[p]) return false;
    g_dsp_cur_pair = p;
    return true;
}

int dsp_pair_count(void)
{
    int n = g_geom.rx_ch / 2;
    if (n < 1) n = 1;
    if (n > AUDIO_MAX_PAIRS) n = AUDIO_MAX_PAIRS;
    return n;
}

// ----------------------------------------------------------
// Meter (framework): analyzer effect -> LED needle / web status
// ----------------------------------------------------------
// Single writer (DSP task); aligned 32-bit writes are atomic on Xtensa. seq
// is written last, so a reader that sees a new seq has fresh value/conf (a
// torn trio across one publish is harmless for display).
static volatile float    s_meter_val  = 0.0f;
static volatile float    s_meter_conf = 0.0f;
static volatile uint32_t s_meter_seq  = 0;

void dsp_meter_publish(float value, float conf)
{
    if (value < -1.0f) value = -1.0f;
    if (value >  1.0f) value =  1.0f;
    if (conf  <  0.0f) conf  =  0.0f;
    if (conf  >  1.0f) conf  =  1.0f;
    s_meter_val  = value;
    s_meter_conf = conf;
    s_meter_seq  = s_meter_seq + 1;   // published last
}

void dsp_meter_read(float *value, float *conf, uint32_t *seq)
{
    if (value) *value = s_meter_val;
    if (conf)  *conf  = s_meter_conf;
    if (seq)   *seq   = s_meter_seq;
}

// ============================================================================
// Bypass
// ============================================================================
static void fx_bypass(float xL, float xR, float *yL, float *yR)
{
    *yL = xL;
    *yR = xR;
}

// ============================================================================
// The registry (registration order defines the Feature-button cycle)
// ============================================================================
static const dsp_effect_t s_builtin_fx[] = {
    { .name = "bypass",  .label = "Bypass",                  // index 0: dsp_is_bypass()
      .process = fx_bypass },
};
#define BUILTIN_FX_COUNT ((int)(sizeof(s_builtin_fx) / sizeof(s_builtin_fx[0])))

// Registered effects: channel first (dsp_effects_init), then the
// out-of-tree ones (rs_user_effects_init contract in the header).
// Appended after bypass in the unified index space, so an existing
// g_state.dsp_effect index never shifts when a component registers.
static const dsp_effect_t *s_user_fx[DSP_USER_EFFECT_SLOTS];
static int                 s_user_fx_count = 0;

bool dsp_effects_register(const dsp_effect_t *fx)
{
    if (!fx || !fx->name || !fx->process) return false;
    if (s_user_fx_count >= DSP_USER_EFFECT_SLOTS) {
        ESP_LOGE(TAG, "effect table full (%d slots) — '%s' not registered",
                 DSP_USER_EFFECT_SLOTS, fx->name);
        return false;
    }
    if (dsp_find(fx->name) >= 0) {
        ESP_LOGE(TAG, "effect name '%s' already registered", fx->name);
        return false;
    }
    s_user_fx[s_user_fx_count++] = fx;
    ESP_LOGI(TAG, "registered effect '%s'", fx->name);
    return true;
}

// Weak default: with the effects component removed, the firmware still builds
// and runs (bypass + channel only). components/rs_student_fx/ overrides this.
__attribute__((weak)) void rs_user_effects_init(void) { }

int dsp_effect_count(void) { return BUILTIN_FX_COUNT + s_user_fx_count; }

const dsp_effect_t *dsp_effect_at(int idx)
{
    if (idx < 0 || idx >= dsp_effect_count()) return NULL;
    return (idx < BUILTIN_FX_COUNT) ? &s_builtin_fx[idx]
                                    : s_user_fx[idx - BUILTIN_FX_COUNT];
}

// ============================================================================
// Registry helpers
// ============================================================================
const dsp_effect_t *dsp_current(void)
{
    const dsp_effect_t *fx = dsp_effect_at(g_state.dsp_effect);
    return fx ? fx : &s_builtin_fx[0];
}

bool dsp_is_bypass(const dsp_effect_t *fx) { return fx == &s_builtin_fx[0]; }

const char *dsp_effect_group(const dsp_effect_t *fx)
{
    return fx->group ? fx->group : fx->name;
}

uint32_t dsp_effect_led(const dsp_effect_t *fx)
{
    if (dsp_is_bypass(fx)) return 0;
    return fx->led_rgb ? (fx->led_rgb & 0xFFFFFFu) : DSP_LED_RGB_DEFAULT;
}

int dsp_led_clash(const dsp_effect_t *fx)
{
    if (dsp_is_bypass(fx)) return -1;
    const uint32_t rgb = dsp_effect_led(fx);
    const char *group  = dsp_effect_group(fx);
    for (int i = 0; i < dsp_effect_count(); i++) {
        const dsp_effect_t *o = dsp_effect_at(i);
        if (o == fx || dsp_is_bypass(o)) continue;
        if (strcmp(dsp_effect_group(o), group) == 0) continue;   // same group may share
        if (dsp_effect_led(o) == rgb) return i;
    }
    return -1;
}

// True when no earlier registry entry belongs to the same group as entry idx.
static bool first_of_group(int idx)
{
    const char *group = dsp_effect_group(dsp_effect_at(idx));
    for (int i = 0; i < idx; i++) {
        if (strcmp(dsp_effect_group(dsp_effect_at(i)), group) == 0) return false;
    }
    return true;
}

// One warning per pair of groups that resolve to the same Mode LED color.
// Boot logs before the USB console is up are not visible there, so `mode`
// flags the same clashes on demand.
static void dsp_check_led_clashes(void)
{
    for (int i = 0; i < dsp_effect_count(); i++) {
        const dsp_effect_t *a = dsp_effect_at(i);
        if (dsp_is_bypass(a) || !first_of_group(i)) continue;
        for (int j = i + 1; j < dsp_effect_count(); j++) {
            const dsp_effect_t *b = dsp_effect_at(j);
            if (!first_of_group(j)) continue;
            if (dsp_effect_led(a) == dsp_effect_led(b)) {
                ESP_LOGW(TAG, "groups '%s' and '%s' share Mode LED color #%06lX: "
                         "give one of them a distinct led_rgb",
                         dsp_effect_group(a), dsp_effect_group(b),
                         (unsigned long)dsp_effect_led(a));
            }
        }
    }
}

int dsp_find(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < dsp_effect_count(); i++) {
        if (strcmp(name, dsp_effect_at(i)->name) == 0) return i;
    }
    // Legacy names second, so a real name always wins over an alias.
    for (int i = 0; i < dsp_effect_count(); i++) {
        const char *alias = dsp_effect_at(i)->alias;
        if (alias && strcmp(name, alias) == 0) return i;
    }
    return -1;
}

void dsp_set(int idx)
{
    if (idx >= 0 && idx < dsp_effect_count()) g_state.dsp_effect = idx;
}

void dsp_current_submode(int *pos, int *count)
{
    const dsp_effect_t *cur = dsp_current();
    const char *group = dsp_effect_group(cur);
    int p = 0, c = 0;
    for (int i = 0; i < dsp_effect_count(); i++) {
        const dsp_effect_t *fx = dsp_effect_at(i);
        if (strcmp(dsp_effect_group(fx), group) == 0) {
            if (fx == cur) p = c;
            c++;
        }
    }
    if (pos)   *pos   = p;
    if (count) *count = c;
}

void dsp_effects_init(void)
{
    // Every pair this boot's geometry carries runs the sample loop, even when
    // an effect's per-pair allocation failed (that effect passes audio
    // through on the pair).
    int pairs = dsp_pair_count();
    for (int p = 0; p < pairs; p++) s_pair_ok[p] = true;

    // Channel registers with the same two calls a sandbox effect makes in
    // rs_user_effects_init(). It goes first, so it is always index 1, right
    // after bypass.
    channel_fx_init();
    dsp_effects_register(&g_fx_channel);

    // Out-of-tree effects register here — before settings_store_init applies
    // a saved default, so a persisted effect resolves by name at boot. Their
    // init code may allocate per-pair state (g_geom is final by now).
    rs_user_effects_init();

    dsp_check_led_clashes();
}

void dsp_effects_reset_all(void)
{
    for (int i = 0; i < dsp_effect_count(); i++) {
        if (dsp_effect_at(i)->reset) dsp_effect_at(i)->reset();
    }
    ESP_LOGI(TAG, "State flushed");
}
