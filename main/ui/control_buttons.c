// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#include "control_buttons.h"
#include "control_pot.h"
#include "common.h"
#include "dsp_effects.h"
#include "audio_codec.h"
#include "power_sleep.h"
#include "settings_store.h"
#include "nvs.h"          // ESP_ERR_NVS_NOT_FOUND (settings load fallback)
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ctrl_btn";

// There is no explicit debounce: the 20 ms poll spacing IS the debounce —
// switch bounce shorter than one poll period can't produce two edges, and
// these tact switches settle well inside it. If a double-fire ever shows on
// hardware, add a two-sample confirm rather than shortening the period.
#define BTN_POLL_MS      20
#define SLEEP_HOLD_MS    1750   // btn1 long-hold enters sleep
#define SETTINGS_HOLD_MS 1750   // btn5 long-hold saves the power-on default
#define RPT_DELAY_MS     400    // gain buttons: hold this long to auto-repeat
#define RPT_PERIOD_MS    150    // ...then one step per period

// ==========================================================
// Button assignment (issue #8): the BOOT button is a shift (hold) modifier
// giving each top-row button two functions. Buttons are addressed by PANEL
// POSITION (btn1..btn5, left to right) because the silkscreen refdes were
// renumbered between Rev 2 and Rev 3 — the position→silk mapping lives at
// hal_panel_buttons() in hal_rev2.c.
//
//   Button | Rev2/Rev3 silk | Normal              | shift held
//   -------+----------------+---------------------+------------------------
//   btn1   | SW6 / SW3      | feature select      | sub-mode select
//          |                | (long-hold: sleep; btn1 also wakes)
//   btn2   | SW3 / SW4      | bypass toggle       | input select (line/mic)
//   btn3   | SW7 / SW5      | input gain +1 dB    | input gain -1 dB
//   btn4   | SW4 / SW6      | output gain +1 dB   | output gain -1 dB
//   btn5   | SW5 / SW7      | save settings       | load settings
//          |                | (long-hold: save current as the power-on default)
//
// The feature/sub-mode structure is derived at init from the DSP effect
// registry (dsp_effect_at/dsp_effect_count: bypass plus every registered
// effect). A feature is one effect group (dsp_effect_t.group): the Feature
// action cycles the groups in registry order, Sub-mode cycles the effects
// within the current group. A registered effect joins automatically, with
// no edits in this file.
// ==========================================================

#define MAX_FEATURES  DSP_USER_EFFECT_SLOTS   // every effect could be its own group
#define MAX_SUBS      DSP_USER_EFFECT_SLOTS   // or all of them in one group

static struct {
    const char   *group;           // dsp_effect_group() of the members
    int           idx[MAX_SUBS];   // effect indices in this feature (registry order)
    int           count;
} s_feat[MAX_FEATURES];
static int  s_feat_count = 0;      // number of non-bypass features
static int  s_bypass_idx = -1;     // effect index of the bypass effect

static int  s_cur_feat = 0;             // current feature cursor
static int  s_sub[MAX_FEATURES] = {0};  // remembered sub-mode per feature
static bool s_started  = false;         // first feature press selects feature 0

static hal_panel_buttons_t s_map;
static bool s_available = false;

// Build the feature -> effect-index map from the registry (once at init).
// A feature is one effect group (dsp_effect_t.group, default the name), in
// order of first appearance in the registry.
static void build_feature_map(void)
{
    for (int i = 0; i < dsp_effect_count(); i++) {
        const dsp_effect_t *fx = dsp_effect_at(i);
        if (dsp_is_bypass(fx)) { s_bypass_idx = i; continue; }
        const char *g = dsp_effect_group(fx);

        int b = -1;
        for (int j = 0; j < s_feat_count; j++) if (strcmp(s_feat[j].group, g) == 0) { b = j; break; }
        if (b < 0 && s_feat_count < MAX_FEATURES) {
            b = s_feat_count++;
            s_feat[b].group = g;
            s_feat[b].count = 0;
        }
        // A registered effect joins the panel automatically, so a failure to
        // join must not be silent: an effect past the table caps never
        // appears on the buttons.
        if (b < 0) {
            ESP_LOGW(TAG, "feature map full (MAX_FEATURES=%d) — '%s' not on the panel",
                     MAX_FEATURES, dsp_effect_at(i)->name);
        } else if (s_feat[b].count >= MAX_SUBS) {
            ESP_LOGW(TAG, "sub-mode table full (MAX_SUBS=%d) — '%s' not on the panel",
                     MAX_SUBS, dsp_effect_at(i)->name);
        } else {
            s_feat[b].idx[s_feat[b].count++] = i;
        }
    }
}

// --- Button actions (each mirrors the equivalent CLI command) ---

// Last effect index THIS module applied — the staleness sentinel for the lazy
// re-sync below (-1 = buttons haven't applied anything yet).
static int s_last_applied_idx = -1;

// Apply the effect for the current feature + sub-mode selection.
static void apply_current(void)
{
    if (s_feat_count == 0) return;
    int idx = s_feat[s_cur_feat].idx[s_sub[s_cur_feat]];
    dsp_set(idx);
    s_last_applied_idx = idx;
    ESP_LOGI(TAG, "DSP: %s", dsp_effect_at(idx)->label);
}

// Lazy cursor re-sync: if something OTHER than the buttons changed the effect
// since we last applied one (CLI `mode`, a web request, a settings load), pick
// up from the current effect instead of the stale cursor. This is what lets
// settings_store stay ignorant of this module (no call-back, no include cycle).
static void resync_if_external_change(void)
{
    if (g_state.dsp_effect != s_last_applied_idx) {
        control_buttons_sync();
        s_last_applied_idx = g_state.dsp_effect;
    }
}

// Cycle to the next feature group, in registry order.
static void action_feature(void)
{
    if (s_feat_count == 0) return;
    resync_if_external_change();
    if (!s_started) {
        s_started = true;            // first press selects the first feature
    } else {
        s_cur_feat = (s_cur_feat + 1) % s_feat_count;
    }
    apply_current();
}

// Cycle the sub-mode within the current feature (no-op cycle for a feature
// with a single sub-mode, e.g. Channel).
static void action_submode(void)
{
    if (s_feat_count == 0) return;
    resync_if_external_change();
    int f = s_cur_feat;
    s_sub[f] = (s_sub[f] + 1) % s_feat[f].count;
    s_started = true;
    apply_current();
}

// Toggle the input source between line-in (differential) and mic.
static void action_input(void)
{
    if (g_state.input_mode == INPUT_MIC) {
        g_state.input_mode    = INPUT_LINE_DIFF;
        g_state.input_gain_db = CODEC_DEFAULT_LINE_GAIN_DB;
    } else {
        g_state.input_mode    = INPUT_MIC;
        g_state.input_gain_db = CODEC_DEFAULT_MIC_GAIN_DB;
    }
    codec_apply_settings();
    ESP_LOGI(TAG, "input -> %s, gain=%ddB",
             (g_state.input_mode == INPUT_MIC) ? "mic" : "line",
             g_state.input_gain_db);
}

// Toggle pass-thru (bypass). Leaving bypass restores the selected feature.
static void action_passthru(void)
{
    if (dsp_is_bypass(dsp_current())) {
        s_started = true;
        apply_current();
    } else if (s_bypass_idx >= 0) {
        dsp_set(s_bypass_idx);
        ESP_LOGI(TAG, "DSP: Bypass (pass-thru)");
    }
}

// Step input gain (MICPGA, 0..47 dB).
static void action_gain_in(bool up)
{
    int v = g_state.input_gain_db + (up ? 1 : -1);
    if (v < CODEC_INPUT_GAIN_MIN_DB) v = CODEC_INPUT_GAIN_MIN_DB;
    if (v > CODEC_INPUT_GAIN_MAX_DB) v = CODEC_INPUT_GAIN_MAX_DB;
    if (v == g_state.input_gain_db) return;   // at a limit — nothing to do
    g_state.input_gain_db = v;
    codec_apply_input_gain();   // gain-only: 2 SPI writes (this auto-repeats)
    ESP_LOGI(TAG, "input gain -> %d dB", v);
}

// SW5 short press: save settings; shifted: load them back (falls back to the
// power-on default slot if nothing was ever saved). Long-hold is handled in
// the poll loop (save-as-default).
static void action_settings(bool shifted)
{
    if (shifted) {
        esp_err_t err = settings_load(SETTINGS_SLOT_USER);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = settings_load(SETTINGS_SLOT_DEFAULT);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(TAG, "settings: nothing saved yet");
    } else {
        settings_save(SETTINGS_SLOT_USER);
    }
}

// Step output gain (HP/LO drivers, -6..+29 dB).
static void action_gain_out(bool up)
{
    int v = g_state.output_gain_db + (up ? 1 : -1);
    if (v < CODEC_OUTPUT_GAIN_MIN_DB) v = CODEC_OUTPUT_GAIN_MIN_DB;
    if (v > CODEC_OUTPUT_GAIN_MAX_DB) v = CODEC_OUTPUT_GAIN_MAX_DB;
    if (v == g_state.output_gain_db) return;
    g_state.output_gain_db = v;
    codec_apply_output_gain();
    ESP_LOGI(TAG, "output gain -> %d dB", v);
}

// ==========================================================
// Polling task
// ==========================================================
// Per-button state for the two gain buttons (press + hold-to-repeat).
typedef struct {
    gpio_num_t gpio;
    void      (*act)(bool up);
    int        prev;        // last level (1 = released)
    int        held_ms;     // time pressed
    int        next_rpt_ms; // next auto-repeat threshold
} gain_btn_t;

static void control_btn_task(void *arg)
{
    int  prev6 = 1, prev3 = 1, prev5 = 1;
    int  sw6_held_ms  = 0;
    bool sw6_shifted  = false;   // SW2 was held when SW6 went down
    bool sw6_slept    = false;   // long-hold fired — swallow the release edge
    int  sw5_held_ms  = 0;
    bool sw5_shifted  = false;   // SW2 was held when SW5 went down
    bool sw5_fired    = false;   // long-hold fired — swallow the release edge

    gain_btn_t gains[2] = {
        { s_map.btn3, action_gain_in,  1, 0, 0 },
        { s_map.btn4, action_gain_out, 1, 0, 0 },
    };

    for (;;) {
        // SW2 shift layer — sampled fresh each poll so the shifted direction
        // applies per press (and per auto-repeat step) of the other buttons.
        bool shift = (gpio_get_level(s_map.shift) == 0);

        // --- SW6: feature select / sub-mode select / long-hold sleep ---
        int l6 = gpio_get_level(s_map.btn1);
        if (l6 == 0) {
            if (prev6 == 1) {                     // press edge
                sw6_held_ms = 0;
                sw6_shifted = shift;
                sw6_slept   = false;
            }
            sw6_held_ms += BTN_POLL_MS;
            // Long-hold (unshifted only) enters sleep. power_sleep_enter()
            // blocks here until wake and waits for SW6 release, so the
            // release edge below is naturally swallowed via sw6_slept.
            if (!sw6_shifted && !shift && !sw6_slept &&
                sw6_held_ms >= SLEEP_HOLD_MS) {
                sw6_slept = true;
                power_sleep_enter();
            }
        } else if (prev6 == 0 && !sw6_slept) {    // release edge, short press
            if (sw6_shifted) action_submode();
            else             action_feature();
        }
        prev6 = l6;

        // --- SW3: bypass toggle / input select ---
        int l3 = gpio_get_level(s_map.btn2);
        if (l3 == 0 && prev3 == 1) {
            if (shift) action_input();
            else       action_passthru();
        }
        prev3 = l3;

        // --- SW7 / SW4: gain step on press, auto-repeat on hold ---
        for (int i = 0; i < 2; i++) {
            gain_btn_t *b = &gains[i];
            int lvl = gpio_get_level(b->gpio);
            if (lvl == 0) {
                if (b->prev == 1) {               // press edge — immediate step
                    b->held_ms     = 0;
                    b->next_rpt_ms = RPT_DELAY_MS;
                    b->act(!shift);
                } else {
                    b->held_ms += BTN_POLL_MS;
                    if (b->held_ms >= b->next_rpt_ms) {
                        b->next_rpt_ms += RPT_PERIOD_MS;
                        b->act(!shift);
                    }
                }
            }
            b->prev = lvl;
        }

        // --- SW5: save settings / load settings / long-hold save default ---
        int l5 = gpio_get_level(s_map.btn5);
        if (l5 == 0) {
            if (prev5 == 1) {                     // press edge
                sw5_held_ms = 0;
                sw5_shifted = shift;
                sw5_fired   = false;
            }
            sw5_held_ms += BTN_POLL_MS;
            if (!sw5_shifted && !sw5_fired && sw5_held_ms >= SETTINGS_HOLD_MS) {
                sw5_fired = true;                 // save-as-default fires on hold
                settings_save(SETTINGS_SLOT_DEFAULT);
            }
        } else if (prev5 == 0 && !sw5_fired) {    // release edge, short press
            action_settings(sw5_shifted);
        }
        prev5 = l5;

        // --- Analog pot (Rev 3, IO1) ---
        // Rides this loop rather than spawning its own task: 50 Hz is the right
        // sampling rate for a knob, and a second poller would cost another
        // internal-RAM stack for nothing. No-op on boards without a pot.
        control_pot_poll();

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

// Point the feature/sub-mode cursor at the currently active effect, so the
// next feature/sub-mode press cycles onward from it. Called at init (boot may
// have applied a saved default) and lazily from the button actions whenever
// an external effect change is detected (resync_if_external_change). Bypass
// and unknown effects leave the cursor where it was; a no-op before init.
void control_buttons_sync(void)
{
    int cur = g_state.dsp_effect;
    for (int f = 0; f < s_feat_count; f++) {
        for (int s = 0; s < s_feat[f].count; s++) {
            if (s_feat[f].idx[s] == cur) {
                s_cur_feat = f;
                s_sub[f]   = s;
                s_started  = true;
                return;
            }
        }
    }
}

esp_err_t control_buttons_init(void)
{
    s_available = hal_panel_buttons(&s_map);
    if (!s_available) {
        ESP_LOGI(TAG, "no panel buttons on this board — feature disabled");
        return ESP_OK;
    }

    build_feature_map();
    control_buttons_sync();   // boot may have applied a saved default effect

    // GPIOs are configured as pulled-up inputs by hal_buttons_init().
    BaseType_t ok = xTaskCreatePinnedToCore(control_btn_task, "ctrl_btn",
                                            4096, NULL, 4, NULL, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    // Positional + GPIO — silk refdes differ per rev (see the banner table),
    // so the GPIO numbers are the unambiguous part of this line.
    ESP_LOGI(TAG, "panel: shift(G%d) btn1(G%d)=feature/sleep btn2(G%d)=bypass "
                  "btn3(G%d)=gain-in btn4(G%d)=gain-out btn5(G%d)=save/load",
             s_map.shift, s_map.btn1, s_map.btn2, s_map.btn3, s_map.btn4, s_map.btn5);
    return ESP_OK;
}
