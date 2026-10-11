// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// Stuck-input guard — the field backstop for the LINE IN "no input" fault.
//
// audio_codec.c precharges the inputs to common mode before every routing
// change, which removes the trigger we can reproduce (routing onto uncharged
// coupling caps). It cannot stop something knocking already-routed pins off
// CM — a source's turn-on pop, overdrive, ESD — and the stuck state was also
// seen after power-ups (Rev 3 DVT, Rev A) with the cause unproven. So this
// watches for the state's signature and runs the recovery proven on the live
// fault (un-route, hold every pin at CM, re-route).
//
// Signature, measured on Rev 3 U1 and Rev A SN 0234 (0.9 V CM, gains 0-20 dB):
// the inputs are AC-coupled, so the ADC's DC is the codec's own — -0.0051 to
// +0.0017 FS on every healthy capture, 0.016 to 0.19 FS (either sign, per
// channel) when stuck, steady to ~2% from window to window; a stuck channel
// carries ~-70 dBFS of noise; the partial state (Rev 3 U1, 8/10) passed signal
// with DC at -0.04..-0.06 FS. A channel counts as stuck when its DC is steady
// across consecutive polls (an RC tail after unplugging a source decays ~37%
// per poll and never qualifies) and either
//   - |DC| > GUARD_DC_LIMIT with AC level below GUARD_RMS_MAX (a loud real
//     signal can't be a stuck input, and clipping biases the mean), or
//   - |DC| > GUARD_DC_HARD with the window peak below GUARD_PEAK_MAX (catches
//     the partial state under program material; the peak test keeps clipping
//     out),
// for GUARD_BAD_POLLS polls in a row, outside the settle window after any
// routing change.
//
// LINE IN (diff) ONLY. Measured 2026-09-28 on Rev A SN 0234 at 0.75 V CM, no
// source: diff-mode DC is flat with gain (-0.0045 FS at 0-30 dB, -0.0063 at
// 47 dB), but mic-mode DC scales with it (+0.0023 at 20 dB, +0.0139 at 40,
// +0.0272 at 47 — the mic PGA runs single-ended against the internal CM, so a
// small bias difference is amplified), which would trip the rules at 40 dB+.
// The fault has only ever been seen in diff mode, so mic mode is reported in
// `status` but never judged.
//
// Self-limiting: at most one recovery per GUARD_MIN_GAP_US. If recoveries leave
// the DC where it was GUARD_NOCHANGE_LIMIT times running, that offset is the
// board's own: the guard stands down until the next user routing change (input
// switch, re-init) or `codec guard on`.
#include "input_guard.h"
#include "audio_codec.h"
#include "audio_pipeline.h"
#include "common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "in_guard";

#define FULL_SCALE           2147483648.0f
#define GUARD_POLL_MS        500
#define GUARD_STACK          4096   // logs go through newlib vfprintf twice (unbuffered stdout)
#define GUARD_PRIO           2
#define GUARD_DC_LIMIT       (0.010f * FULL_SCALE)
#define GUARD_RMS_MAX        (0.010f * FULL_SCALE)    // -40 dBFS
#define GUARD_DC_HARD        (0.030f * FULL_SCALE)    // 6x the worst healthy offset seen
#define GUARD_PEAK_MAX       (0.900f * FULL_SCALE)
#define GUARD_STEADY_REL     0.10f                    // |DC - previous poll| <= 10% ...
#define GUARD_STEADY_ABS     (0.001f * FULL_SCALE)    // ... or 0.001 FS
#define GUARD_BAD_POLLS      4                        // ~2 s of consecutive stuck readings
#define GUARD_SETTLE_US      (2000 * 1000LL)          // after any routing change / re-init / wake
#define GUARD_MIN_GAP_US     (30 * 1000000LL)
#define GUARD_NOCHANGE_LIMIT 2
#define GUARD_SAME_DC        (0.002f * FULL_SCALE)    // "the recovery changed nothing"

static volatile bool     s_enabled = true;
static volatile bool     s_stood_down;
static volatile uint32_t s_rearm_gen;      // `codec guard on` bumps it: task resets its counters
static volatile bool     s_test_pending;   // `codec guard test`: run the recovery branch once
static volatile uint32_t s_recoveries;
static volatile bool     s_have, s_unmonitored;
static int32_t           s_mean[2], s_rms[2], s_peak[2];
static TaskHandle_t      s_task;

static bool steady(int32_t mean, int32_t prev)
{
    float d = fabsf((float)mean - (float)prev);
    return d <= GUARD_STEADY_ABS || d <= GUARD_STEADY_REL * fabsf((float)mean);
}

static bool channel_stuck(int c, const int32_t mean[2], const int32_t rms[2],
                          const int32_t peak[2], const int32_t prev[2])
{
    float dc = fabsf((float)mean[c]);
    if (!steady(mean[c], prev[c])) return false;
    return (dc > GUARD_DC_LIMIT && (float)rms[c]  < GUARD_RMS_MAX) ||
           (dc > GUARD_DC_HARD  && (float)peak[c] < GUARD_PEAK_MAX);
}

static float to_dbfs(int32_t rms)
{
    return rms > 0 ? 20.0f * log10f((float)rms / FULL_SCALE) : -150.0f;
}

// True only for a window newer than *seq (false: pipeline parked for sleep,
// TDM 8x16, or no new window yet). Keeps a copy for `status`.
static bool read_fresh(uint32_t *seq, int32_t mean[2], int32_t rms[2], int32_t peak[2])
{
    uint32_t s;
    if (!audio_pipeline_input_stats(mean, rms, peak, &s)) { s_unmonitored = true; return false; }
    s_unmonitored = false;
    if (s == *seq) return false;
    *seq = s;
    for (int c = 0; c < 2; c++) { s_mean[c] = mean[c]; s_rms[c] = rms[c]; s_peak[c] = peak[c]; }
    s_have = true;
    return true;
}

static void guard_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0, epoch = codec_input_epoch(), own_epoch = 0, gen = s_rearm_gen;
    int64_t settle_until = esp_timer_get_time() + GUARD_SETTLE_US;
    int64_t last_recovery = 0;
    int32_t prev[2] = { 0, 0 };
    int bad = 0, nochange = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(GUARD_POLL_MS));
        if (gen != s_rearm_gen) { gen = s_rearm_gen; nochange = 0; bad = 0; last_recovery = 0; }
        int32_t mean[2], rms[2], peak[2];
        if (!read_fresh(&seq, mean, rms, peak)) continue;
        int64_t now = esp_timer_get_time();

        uint32_t ep = codec_input_epoch();
        if (ep != epoch || codec_input_routing_busy()) {   // (re)routed: let it settle
            if (ep != epoch && ep != own_epoch && s_stood_down) {
                s_stood_down = false;                      // the user changed the setup: re-arm
                nochange = 0;
                ESP_LOGI(TAG, "re-armed after a routing change");
            }
            epoch = ep;
            settle_until = now + GUARD_SETTLE_US;
            bad = 0;
            prev[0] = mean[0]; prev[1] = mean[1];
            continue;
        }
        bool test = s_test_pending;
        if (!test && g_state.input_mode != INPUT_LINE_DIFF) {   // mic mode: see header
            bad = 0;
            prev[0] = mean[0]; prev[1] = mean[1];
            continue;
        }
        bool stuck = channel_stuck(0, mean, rms, peak, prev) || channel_stuck(1, mean, rms, peak, prev);
        prev[0] = mean[0]; prev[1] = mean[1];
        if (!test) {
            if (now < settle_until || !s_enabled || s_stood_down) { bad = 0; continue; }
            bad = stuck ? bad + 1 : 0;
            if (bad < GUARD_BAD_POLLS) continue;
            if (last_recovery && now - last_recovery < GUARD_MIN_GAP_US) continue;
        }
        s_test_pending = false;

        const int32_t before[2] = { mean[0], mean[1] };
        if (!codec_recover_inputs_if(epoch)) {   // something else routed/slept meanwhile
            bad = 0;
            continue;
        }
        ESP_LOGW(TAG, "%s (DC L %+.4f R %+.4f FS, level L %.0f R %.0f dBFS): ran codec recover",
                 test ? "TEST: forced recovery" : "input looked stuck",
                 before[0] / FULL_SCALE, before[1] / FULL_SCALE, to_dbfs(rms[0]), to_dbfs(rms[1]));
        if (!test) s_recoveries++;
        last_recovery = esp_timer_get_time();
        epoch = own_epoch = codec_input_epoch();
        bad = 0;

        // Judge it on a window taken after the inputs have settled.
        vTaskDelay(pdMS_TO_TICKS(GUARD_SETTLE_US / 1000 + 2 * GUARD_POLL_MS));
        if (!read_fresh(&seq, mean, rms, peak)) continue;
        prev[0] = mean[0]; prev[1] = mean[1];
        settle_until = 0;
        bool still = channel_stuck(0, mean, rms, peak, prev) || channel_stuck(1, mean, rms, peak, prev);
        if (!still) {
            nochange = 0;
            ESP_LOGW(TAG, "input OK after recovery (DC L %+.4f R %+.4f FS)",
                     mean[0] / FULL_SCALE, mean[1] / FULL_SCALE);
            continue;
        }
        bool same = fabsf((float)mean[0] - (float)before[0]) < GUARD_SAME_DC &&
                    fabsf((float)mean[1] - (float)before[1]) < GUARD_SAME_DC;
        nochange = same ? nochange + 1 : 0;
        ESP_LOGW(TAG, "input still out of range after recovery (DC L %+.4f R %+.4f FS%s)",
                 mean[0] / FULL_SCALE, mean[1] / FULL_SCALE, same ? ", unchanged" : "");
        if (nochange >= GUARD_NOCHANGE_LIMIT) {
            s_stood_down = true;
            ESP_LOGW(TAG, "recovery changed nothing %d times: treating this DC as the board's own "
                          "offset; guard stood down until the next input change or `codec guard on`",
                     GUARD_NOCHANGE_LIMIT);
        }
    }
}

void input_guard_start(void)
{
    if (s_task) return;
    // Core 0 like every non-audio task (task map in radiosonic_main.c).
    xTaskCreatePinnedToCore(guard_task, "in_guard", GUARD_STACK, NULL, GUARD_PRIO, &s_task, 0);
}

void input_guard_enable(bool on)
{
    s_enabled = on;
    if (on) {
        s_stood_down = false;
        s_rearm_gen++;
    }
}

void input_guard_test(void) { s_test_pending = true; }

void input_guard_print_status(void)
{
    const char *st = !s_enabled ? "off" : s_stood_down ? "STOOD DOWN"
                   : (g_state.input_mode == INPUT_LINE_DIFF) ? "on" : "on (idle: judges line in only)";
    if (s_unmonitored) {
        printf("Guard:  input guard %s — not monitored in TDM 8x16\n", st);
    } else if (!s_have) {
        printf("Guard:  input guard %s (no input statistics yet)\n", st);
    } else {
        printf("Guard:  input guard %s, %lu recoveries | input DC L %+.4f R %+.4f FS, level L %.0f R %.0f dBFS\n",
               st, (unsigned long)s_recoveries,
               s_mean[0] / FULL_SCALE, s_mean[1] / FULL_SCALE, to_dbfs(s_rms[0]), to_dbfs(s_rms[1]));
    }
}

unsigned input_guard_stack_free(void)
{
    return s_task ? (unsigned)uxTaskGetStackHighWaterMark(s_task) : 0;
}

unsigned input_guard_stack_size(void) { return GUARD_STACK; }
