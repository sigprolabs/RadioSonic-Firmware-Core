// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#include "control_pot.h"
#include "common.h"
#include "audio_codec.h"
#include "dsp_effects.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "pot";

// ---- Tunables ---------------------------------------------------------------
// The S3 SAR is 12-bit (0..4095 counts). Knob positions use the same 0..4095
// scale, with 4095 at the clockwise stop.
#define POT_RAW_MAX     4095

// The wiper never reaches the ADC's full scale. R12 (1.5 k) sits between 3.3 V
// and the top of the 10 k pot, so the clockwise stop is 3.3 V x 10 / 11.5 =
// 2.87 V, about 87 % of the range (issue #19; Rev A SN 0234 reads raw 3565,
// 2893 mV there). Positions are scaled to that stop, from calibrated
// millivolts when the chip has an ADC calibration scheme (raw counts per volt
// vary chip to chip) and from raw counts otherwise.
#define POT_TOP_MV      2870
#define POT_TOP_RAW     3561    // 87 % of 4095, the no-calibration fallback

// Smoothing: one-pole EMA in 1/16-count fixed point, y += (x - y)/4. At the
// 20 ms ctrl_btn poll that is a ~80 ms time constant — fast enough to feel
// direct, slow enough to bury SAR noise.
#define POT_EMA_SHIFT   2
#define POT_FIX_SHIFT   4   // fixed-point scale of s_ema_x16 (1/16th-count resolution)

// Dead-band, in position counts, that the FILTERED reading must move before
// the pot writes anything. This is what makes takeover soft: a knob sitting
// still never overrides a value set elsewhere (`channel`, a `settings load`)
// or jumps the volume when the target is selected. It must stay comfortably
// above the residual ADC noise floor (~±10 LSB after the EMA).
#define POT_DEADBAND    32

// Travel this close to either stop snaps to it. At the bottom the snap absorbs
// SAR offset and noise near 0 V. At the top it is wider because the
// stop moves with the pot's ±20 % tolerance: an 8 k part stops at 2.78 V, 3 %
// short of nominal, and still has to reach full volume.
#define POT_EDGE_SNAP   64
#define POT_TOP_SNAP    140

// ---- State ------------------------------------------------------------------
static adc_oneshot_unit_handle_t s_adc  = NULL;
static adc_cali_handle_t         s_cali = NULL;
static adc_channel_t             s_chan;
static gpio_num_t                s_gpio = GPIO_NUM_NC;

static pot_target_t s_target = POT_TARGET_OFF;

static int  s_ema_x16 = -1;    // filtered reading, 1/16 counts; -1 = no sample yet
static int  s_last    = -1;    // position at the last apply (dead-band anchor)

// ---- Helpers ----------------------------------------------------------------

// Filtered knob position, 0 (CCW stop) .. POT_RAW_MAX (CW stop) with the end
// stops snapped, or -1 before the first poll.
static int pot_position(void)
{
    if (s_ema_x16 < 0) return -1;
    int raw = s_ema_x16 >> POT_FIX_SHIFT;
    int mv, v;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK)
        v = mv * POT_RAW_MAX / POT_TOP_MV;
    else
        v = raw * POT_RAW_MAX / POT_TOP_RAW;
    if (v <= POT_EDGE_SNAP)              v = 0;
    if (v >= POT_RAW_MAX - POT_TOP_SNAP) v = POT_RAW_MAX;
    return v;
}

// Push the knob position to the selected target. Each branch re-checks the
// QUANTISED value before writing, so a slow sweep costs one SPI write pair per
// 0.5 dB (or one config publish per sample of delay), not one per poll.
static void pot_apply(int val)
{
    switch (s_target) {
    case POT_TARGET_VOLUME: {
        // Linear in dB over the travel: CCW stop -63.5 dB (silent for any
        // practical source), CW stop 0 dB, where `gain out` sets the level.
        int span = CODEC_DAC_VOL_MAX - CODEC_DAC_VOL_MIN;
        int vol  = CODEC_DAC_VOL_MIN + (val * span + POT_RAW_MAX / 2) / POT_RAW_MAX;
        if (vol == codec_get_dac_volume()) return;
        codec_set_dac_volume(vol);
        break;
    }
    case POT_TARGET_FX: {
        // Route the knob to whatever the current effect exposes (its pot_set
        // hook does its own quantizing/locking). An effect without a binding
        // ignores the knob — `pot` prints which case applies.
        const dsp_effect_t *fx = dsp_current();
        if (fx->pot_set) fx->pot_set((float)val / (float)POT_RAW_MAX);
        break;
    }
    case POT_TARGET_OFF:
    default:
        break;
    }
}

// ---- Public API -------------------------------------------------------------

adc_oneshot_unit_handle_t control_pot_adc_unit(void) { return s_adc; }

esp_err_t control_pot_init(void)
{
    if (!hal_pot(&s_gpio)) {
        ESP_LOGI(TAG, "no pot on this board — feature disabled");
        return ESP_OK;
    }

    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(s_gpio, &unit, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not ADC-capable — pot disabled", (int)s_gpio);
        return ESP_OK;   // a board-description error must not stop the boot
    }
    if (unit != ADC_UNIT_1) {
        // ADC2 shares the SAR block with the WiFi PHY: reads start failing the
        // moment WiFi comes up, which would make the knob die mid-session.
        ESP_LOGE(TAG, "GPIO%d is on ADC%d — the pot must be on ADC1 (GPIO1-10); "
                      "disabled", (int)s_gpio, unit + 1);
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 init failed: %s — pot disabled", esp_err_to_name(err));
        s_adc = NULL;
        return ESP_OK;
    }
    adc_oneshot_chan_cfg_t ccfg = {
        .atten    = ADC_ATTEN_DB_12,      // full 0..~3.1 V wiper swing
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, s_chan, &ccfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 ch%d config failed: %s — pot disabled",
                 s_chan, esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return ESP_OK;
    }

    // Calibration is display-only (the `pot read` millivolts); mapping works
    // off raw counts, so a board without a cal scheme loses nothing.
    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id  = unit,
        .chan     = s_chan,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_cali) != ESP_OK) s_cali = NULL;

    ESP_LOGI(TAG, "pot on GPIO%d (ADC1 ch%d), target=off", (int)s_gpio, s_chan);
    return ESP_OK;
}

void control_pot_poll(void)
{
    if (!s_adc) return;

    int raw;
    if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) return;

    if (s_ema_x16 < 0) s_ema_x16 = raw << POT_FIX_SHIFT;
    else               s_ema_x16 += ((raw << POT_FIX_SHIFT) - s_ema_x16) >> POT_EMA_SHIFT;

    int val = pot_position();

    // Track the anchor while disarmed so selecting a target does not instantly
    // fire on stale travel; the knob still has to move to take over.
    if (s_target == POT_TARGET_OFF) { s_last = val; return; }

    if (s_last >= 0 && abs(val - s_last) < POT_DEADBAND) return;
    s_last = val;
    pot_apply(val);
}

void control_pot_set_target(pot_target_t t)
{
    s_target = t;
    s_last   = pot_position();   // re-arm: require movement before taking over
}

// ---- Console command --------------------------------------------------------

static const char *target_name(pot_target_t t)
{
    switch (t) {
    case POT_TARGET_VOLUME: return "volume";
    case POT_TARGET_FX:     return "fx";
    default:                return "off";
    }
}

void control_pot_print_status(void)
{
    if (!s_adc) return;
    int val = pot_position();
    printf("Pot:    GPIO%d -> %s", (int)s_gpio, target_name(s_target));
    if (val >= 0) printf("  (knob %d%%)", (val * 100 + POT_RAW_MAX / 2) / POT_RAW_MAX);
    printf("  volume %.1f dB\n", codec_get_dac_volume() / 2.0f);
}

static void pot_print(void)
{
    if (!s_adc) {
        printf("Pot: not available on this board\n");
        return;
    }
    int val = pot_position();
    printf("Pot:    GPIO%d (ADC1 ch%d)  target=%s\n",
           (int)s_gpio, s_chan, target_name(s_target));
    if (val < 0) {
        printf("Knob:   not sampled yet\n");
    } else {
        printf("Knob:   %d / %d  (%d%%)\n", val, POT_RAW_MAX,
               (val * 100 + POT_RAW_MAX / 2) / POT_RAW_MAX);
    }
    printf("Volume: maps %.1f..%.1f dB, now %.1f dB  (DAC volume; `gain out` %d dB "
           "sets the level at full volume)\n",
           CODEC_DAC_VOL_MIN / 2.0f, CODEC_DAC_VOL_MAX / 2.0f,
           codec_get_dac_volume() / 2.0f, g_state.output_gain_db);
    const dsp_effect_t *fx = dsp_current();
    printf("Fx:     current effect '%s' %s%s%s\n", fx->name,
           fx->pot_set ? "binds the knob to " : "has no pot binding",
           fx->pot_set ? fx->pot_label : "",
           fx->pot_set ? " (see the effect's own command for tuning)" : "");
}

// pot                      — show state
// pot off | volume | fx    — select target (fx = current effect's binding)
// pot apply                — push the knob's current position now (skip takeover)
// pot read                 — raw / filtered / mV diagnostic
static int cmd_pot(int argc, char **argv)
{
    const char *usage = "Usage: pot [off|volume|fx] | pot apply | pot read\n";

    if (!s_adc && argc >= 2) {
        printf("Pot not available on this board\n");
        return 1;
    }
    if (argc < 2) { pot_print(); return 0; }

    if (strcmp(argv[1], "off") == 0) {
        control_pot_set_target(POT_TARGET_OFF);
        printf("Pot: off (knob drives nothing)\n");
        return 0;
    }
    if (strcmp(argv[1], "volume") == 0 || strcmp(argv[1], "vol") == 0) {
        control_pot_set_target(POT_TARGET_VOLUME);
        printf("Pot: volume, %.1f..%.1f dB. Turn the knob to take over "
               "(currently %.1f dB).\n",
               CODEC_DAC_VOL_MIN / 2.0f, CODEC_DAC_VOL_MAX / 2.0f,
               codec_get_dac_volume() / 2.0f);
        return 0;
    }
    if (strcmp(argv[1], "fx") == 0) {
        const dsp_effect_t *fx = dsp_current();
        control_pot_set_target(POT_TARGET_FX);
        if (fx->pot_set) {
            printf("Pot: %s (%s). Turn the knob to take over.\n",
                   fx->pot_label, fx->name);
        } else {
            printf("Pot: fx — but effect '%s' has no pot binding; the knob "
                   "drives nothing until you switch to one that does.\n",
                   fx->name);
        }
        return 0;
    }
    if (strcmp(argv[1], "apply") == 0) {
        int val = pot_position();
        if (val < 0)                    { printf("Pot not sampled yet\n"); return 1; }
        if (s_target == POT_TARGET_OFF) { printf("Pot target is off\n");   return 1; }
        s_last = val;
        pot_apply(val);
        pot_print();
        return 0;
    }
    if (strcmp(argv[1], "read") == 0) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) {
            printf("ADC read failed\n");
            return 1;
        }
        printf("GPIO%d raw=%d filtered=%d", (int)s_gpio, raw,
               s_ema_x16 < 0 ? -1 : s_ema_x16 >> POT_FIX_SHIFT);
        int mv;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK)
            printf("  =  %d mV", mv);
        int val = pot_position();
        if (val >= 0) printf("  knob %d%%", (val * 100 + POT_RAW_MAX / 2) / POT_RAW_MAX);
        printf("\n");
        return 0;
    }

    printf("%s", usage);
    return 1;
}

void register_pot_cmd(void)
{
    const esp_console_cmd_t c_pot = {
        .command  = "pot",
        .help     = "Analog pot: pot [off|volume|fx] | apply | read  (fx = current effect's binding)",
        .func     = &cmd_pot,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_pot));
}
