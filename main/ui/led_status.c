// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// Status-LED / meter-needle policy — WHAT the strip shows. The strip driver
// (board/rgb_leds.c) only moves pixels and self-stubs on boards without a
// strip, so this module runs unconditionally and no-ops harmlessly on Rev 1.
// Lives in ui/ (not the board layer) because it reads app state: g_state and
// the DSP effect registry. It knows NO individual effect — analyzer effects
// reach the strip only through the framework meter (dsp_meter_publish) or the
// per-effect indicator hook (dsp_effect_t.leds), both defined in dsp_effects.h.
#include "led_status.h"
#include "common.h"
#include "dsp_effects.h"
#include "rgb_leds.h"
#include <stdint.h>

#ifndef RGB_LED_COUNT
#define RGB_LED_COUNT 0    // board without a strip: loops vanish, driver stubs no-op
#endif

// ----------------------------------------------------------
// 4-LED status strip (chain order: LED 1 = index 0 ... LED 4 = index 3)
//   LED 1  Input     mic = yellow, line = cyan
//   LED 2  Bypass    pass-thru on = white, off = dark
//   LED 3  Mode      the color the current effect declares (led_rgb), so
//                    each effect's designer picks its own; unset = dim white,
//                    bypass = dark
//   LED 4  Sub-mode  position within a group of several effects:
//                    1st = green, 2nd and later = orange,
//                    single-effect group / bypass = dark
// While the current effect publishes on the framework meter the whole strip
// becomes a live needle instead (led_render_meter below). An effect with an
// indicator hook paints LEDs 2-4 itself while it is current
// (led_render_indicators below); LED 1 keeps the input color.
// Moderate brightness so the indicators are readable, not blinding.
// ----------------------------------------------------------
#define LED_LVL   90                 // base on-level (0-255)
#define LED_OFF   0, 0, 0
// One full-scale color channel (low 8 bits of v) scaled to LED_LVL.
#define LED_SCALE(v) ((uint8_t)((((v) & 0xFFu) * LED_LVL) / 255u))

// Cached signature of the last status render; -1 forces a repaint.
static int s_led_status_sig = -1;

void led_status_invalidate(void)
{
    s_led_status_sig = -1;
}

// ---- Meter needle ----------------------------------------------------------
// GENERIC: renders the framework meter (dsp_meter_publish, dsp_effects.h) as
// an orange needle swept across the strip — value -1 = leftmost LED, 0 =
// center (between LEDs 2/3), +1 = rightmost, brightness scaled by conf, with
// sub-LED interpolation so motion looks continuous. What the value MEANS
// belongs to the publishing effect (for example, a direction mapped onto
// -1..+1); this module just draws it.
// conf == 0 (no lock) shows a dim blue "armed" glow. The needle runs while
// the meter seq advances and hands back to the status view
// when the analyzer stops publishing (mode change / pipeline parked).
#define METER_LED_EMA_A  0.5f    // display smoothing per 100 ms led_task poll
#define METER_LED_IDLE   6       // dim idle-glow level

// Returns false when no analyzer is publishing (caller shows the status view).
static bool led_render_meter(void)
{
    static float    ema = 0.0f;
    static uint32_t last_seq = 0;
    static int      stale = 6;   // start inactive until a publish arrives

    float val, conf; uint32_t seq;
    dsp_meter_read(&val, &conf, &seq);
    stale = (seq == last_seq) ? stale + 1 : 0;    // DSP idle / no analyzer?
    last_seq = seq;
    if (stale > 5) return false;

    // The static status view is stale while the needle runs — force a full
    // repaint when the analyzer stops.
    s_led_status_sig = -1;

    if (conf <= 0.0f) {                           // armed, no lock -> idle glow
        for (int i = 0; i < RGB_LED_COUNT; i++) rgb_leds_set(i, 0, 0, METER_LED_IDLE);
        rgb_leds_refresh();
        ema = 0.0f;                               // don't smooth across gaps
        return true;
    }

    // Display smoothing on the bounded value (plain EMA — no wrap seam here).
    ema += METER_LED_EMA_A * (val - ema);

    // value in [-1,+1] -> needle position p in [0.0 .. 3.0] across the strip.
    float p = (ema + 1.0f) * 1.5f;
    if (p < 0.0f) p = 0.0f;
    if (p > 3.0f) p = 3.0f;
    int   i0 = (int)p;  if (i0 > 2) i0 = 2;       // adjacent pair (i0, i0+1)
    float fr = p - (float)i0;                     // 0 -> all i0, 1 -> all i0+1

    float lvl = (float)LED_LVL * conf;            // effect-supplied brightness
    float wa = lvl * (1.0f - fr);                 // anti-aliased needle split
    float wb = lvl * fr;
    for (int i = 0; i < RGB_LED_COUNT; i++) rgb_leds_set(i, LED_OFF);
    rgb_leds_set(i0,     (uint8_t)wa, (uint8_t)(wa * 0.5f), 0);   // orange
    rgb_leds_set(i0 + 1, (uint8_t)wb, (uint8_t)(wb * 0.5f), 0);
    rgb_leds_refresh();
    return true;
}

// LED 1 — Input source. Shared by the status view and the indicator view.
static void led_paint_input(void)
{
    if (g_state.input_mode == INPUT_MIC) rgb_leds_set(0, LED_LVL, LED_LVL, 0);   // yellow
    else                                 rgb_leds_set(0, 0, LED_LVL, LED_LVL);   // cyan (line in)
}

// ---- Indicator LEDs ---------------------------------------------------------
// GENERIC: an effect that sets dsp_effect_t.leds paints LEDs 2-4 itself while
// it is current (for example one lamp per detected signal). The hook fills
// three full-scale colors; this module scales them to the strip's brightness
// and paints them, with LED 1 still showing the input source. The strip is
// refreshed only when the colors or the input/effect signature change, so a
// hook that reports a steady state costs nothing per poll.
// Returns false when the current effect has no hook (caller shows the status view).
static bool led_render_indicators(const dsp_effect_t *fx)
{
    static uint32_t last_rgb[3] = { ~0u, ~0u, ~0u };
    static int      last_sig    = -1;

    if (!fx || !fx->leds) { last_sig = -1; return false; }

    uint32_t rgb[3] = { 0, 0, 0 };
    fx->leds(rgb);

    int sig = (g_state.dsp_effect << 4) | (int)g_state.input_mode;
    if (s_led_status_sig == -2 && sig == last_sig && rgb[0] == last_rgb[0] &&
        rgb[1] == last_rgb[1] && rgb[2] == last_rgb[2]) {
        return true;                               // nothing changed
    }
    last_sig = sig;
    for (int i = 0; i < 3; i++) last_rgb[i] = rgb[i];

    // -2 marks the strip as painted by us. The meter and invalidate set -1,
    // which forces this repaint; the status view repaints after us too.
    s_led_status_sig = -2;

    led_paint_input();
    for (int i = 0; i < 3; i++) {
        const uint32_t c = rgb[i];
        rgb_leds_set(i + 1, LED_SCALE(c >> 16), LED_SCALE(c >> 8), LED_SCALE(c));
    }
    rgb_leds_refresh();
    return true;
}

void led_status_render(void)
{
    const dsp_effect_t *fx = dsp_current();

    // A publishing analyzer takes the strip: live needle instead of the
    // static status view, repainted every led_task poll (the signature cache
    // below does not apply).
    if (!(fx && fx->leds) && led_render_meter()) return;

    // An effect with indicator LEDs paints LEDs 2-4 itself.
    if (led_render_indicators(fx)) return;

    // Only repaint when the relevant state changed (cheap to poll).
    int sig = (g_state.dsp_effect << 4) | (int)g_state.input_mode;
    if (sig == s_led_status_sig) return;
    s_led_status_sig = sig;

    const bool bypass = dsp_is_bypass(fx);

    led_paint_input();

    // LED 2 — Bypass / pass-thru
    if (bypass) rgb_leds_set(1, LED_LVL, LED_LVL, LED_LVL);      // white
    else        rgb_leds_set(1, LED_OFF);

    // LED 3 (Mode): the effect's own color at the strip's brightness.
    // dsp_effect_led() returns its led_rgb, 0 (off) for bypass, or dim white
    // when unset. An analyzer's color shows briefly before its first meter
    // publish takes the strip over.
    const uint32_t rgb = dsp_effect_led(fx);
    rgb_leds_set(2, LED_SCALE(rgb >> 16), LED_SCALE(rgb >> 8), LED_SCALE(rgb));

    // LED 4 — Sub-mode within the feature (off when the feature has only one)
    int pos, count;
    dsp_current_submode(&pos, &count);
    if (bypass || count <= 1) rgb_leds_set(3, LED_OFF);
    else if (pos == 0)        rgb_leds_set(3, 0, LED_LVL, 0);            // green (1st)
    else                      rgb_leds_set(3, LED_LVL, LED_LVL / 2, 0);  // orange (2nd)

    rgb_leds_refresh();
}
