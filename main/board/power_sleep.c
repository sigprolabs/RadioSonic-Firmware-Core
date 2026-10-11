// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#include "power_sleep.h"
#include "common.h"
#include "audio_pipeline.h"
#include "audio_codec.h"
#include "audio_store.h"
#include "wifi_manager.h"
#include "cli_console.h"
#include "rgb_leds.h"
#include "led_status.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_console.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "sleep";

#define WAKE_POLL_MS      20
#define BLINK_PERIOD_US   (5ULL * 1000 * 1000)
#define BLINK_PERIOD_MS   ((uint32_t)(BLINK_PERIOD_US / 1000))  // one value, both loops
#define BLINK_ON_MS       100
#define WAKE_DEBOUNCE_MS  20
#define MIC_SETTLE_MS     750    // ADC stays muted while MICBIAS settles
#define FADE_IN_MS        800    // DAC volume ramp back to 0 dB

// Sleep trigger is the btn1 long-hold in control_buttons.c (silk SW3 on Rev 3/A,
// SW6 on Rev 2) or the `sleep` CLI command; this module owns the enter/wake
// sequence and the wake pin.
static gpio_num_t s_wake_btn;
static bool s_available = false;
static volatile bool s_sleeping = false;

// Block until every listed button reads released (high) for 5 consecutive
// 10 ms polls, so a lingering press can't immediately re-trigger anything.
static void wait_for_release(const gpio_num_t *btns, int count)
{
    int stable = 0;
    while (stable < 5) {
        bool all_up = true;
        for (int i = 0; i < count; i++) {
            if (gpio_get_level(btns[i]) == 0) { all_up = false; break; }
        }
        stable = all_up ? stable + 1 : 0;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t power_sleep_enter(void)
{
    if (!s_available) return ESP_ERR_NOT_SUPPORTED;
    if (s_sleeping)   return ESP_ERR_INVALID_STATE;
    s_sleeping = true;

    ESP_LOGI(TAG, "entering light sleep (wake: press button on GPIO%d)", s_wake_btn);

    // --- Quiesce ---
    if (audio_store_is_recording()) audio_store_record_stop();
    if (audio_store_is_playing())   audio_store_play_stop();
    bool wifi_was_on = wifi_manager_suspend();
    codec_sleep();                     // needs I2S still clocking (clean mutes)

    esp_err_t err = audio_pipeline_suspend();
    if (err != ESP_OK) {
        // Tasks didn't park — undo and bail rather than sleep in a bad state.
        ESP_LOGE(TAG, "pipeline suspend failed, aborting sleep");
        codec_wake();
        codec_unmute_adc();          // codec_wake leaves the ADC muted for the normal path's settle
        codec_fade_in(FADE_IN_MS);   // ...and the DAC volume at minimum
        if (wifi_was_on) wifi_manager_resume();
        s_sleeping = false;
        return err;
    }

    rgb_leds_all_off();
    led_status_invalidate();   // full repaint when led_task resumes after wake
    gpio_set_level(BLINK_GPIO, 0);

    // --- Arm wake once the trigger button (btn1, also the wake pin) is released ---
    wait_for_release(&s_wake_btn, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // The USB controllers do not survive light sleep (clocks gate off; the
    // TinyUSB CDC console drops off the bus). So when a USB host is attached
    // (which also means we're externally powered and the power budget doesn't
    // matter), stay awake and just idle + blink: the console stays enumerated
    // the whole time. True light sleep only runs untethered, where there is
    // no console to preserve and battery life actually matters. Detection is
    // the TinyUSB mount state, so a charge-only power bank still gets real
    // light sleep.
    bool tethered = console_usb_attached();
    bool button_wake = false;

    if (tethered) {
        ESP_LOGI(TAG, "USB host attached — idle mode (console stays connected)");
        while (!button_wake) {
            // Idle most of the 5 s period, polling the wake button.
            for (uint32_t t = 0; t < (BLINK_PERIOD_MS - BLINK_ON_MS) && !button_wake;
                 t += WAKE_POLL_MS) {
                vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
                if (gpio_get_level(s_wake_btn) == 0) button_wake = true;
            }
            if (button_wake) break;
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(BLINK_ON_MS));
            gpio_set_level(BLINK_GPIO, 0);
            if (gpio_get_level(s_wake_btn) == 0) button_wake = true;
        }
        vTaskDelay(pdMS_TO_TICKS(WAKE_DEBOUNCE_MS));
    } else {
        // Untethered: real light sleep.
        // NOTE: after an untethered light-sleep cycle the USB-OTG (TinyUSB)
        // console may not enumerate when a host is plugged in later — the
        // OTG controller's light-sleep recovery is untested. Power-cycle to
        // restore the console if needed. (The old USB-Serial-JTAG recovery
        // register pokes were removed: they re-pointed the shared PHY at the
        // USJ controller, which would kill the OTG console deterministically.)
        gpio_wakeup_enable(s_wake_btn, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        while (!button_wake) {
            esp_sleep_enable_timer_wakeup(BLINK_PERIOD_US);
            esp_light_sleep_start();

            if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
                vTaskDelay(pdMS_TO_TICKS(WAKE_DEBOUNCE_MS));
                if (gpio_get_level(s_wake_btn) == 0) button_wake = true;
                // else: glitch — loop re-sleeps
            } else {
                // Timer wake: status blink, then back to sleep.
                gpio_set_level(BLINK_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(BLINK_ON_MS));
                gpio_set_level(BLINK_GPIO, 0);
                // Catch a press that landed inside the blink window.
                if (gpio_get_level(s_wake_btn) == 0) button_wake = true;
            }
        }

        gpio_wakeup_disable(s_wake_btn);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }

    wait_for_release(&s_wake_btn, 1);

    // --- Restore, reverse order ---
    audio_pipeline_clocks_on();        // MCLK back before codec PLL relock
    codec_wake();                      // powers up: ADC muted, DAC volume at minimum
    audio_pipeline_resume();
    vTaskDelay(pdMS_TO_TICKS(MIC_SETTLE_MS)); // MICBIAS transient settles, muted
    codec_unmute_adc();
    codec_fade_in(FADE_IN_MS);         // ramp output back up to the set volume
    if (wifi_was_on) wifi_manager_resume();

    ESP_LOGI(TAG, "woke from light sleep");
    s_sleeping = false;
    return ESP_OK;
}

esp_err_t power_sleep_init(void)
{
    // The sleep *trigger* (btn1 long-hold) lives in control_buttons.c, which
    // calls power_sleep_enter(); this module only owns the wake pin.
    s_available = hal_sleep_buttons(&s_wake_btn);
    if (!s_available) {
        ESP_LOGI(TAG, "no sleep buttons on this board — sleep mode disabled");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "sleep: btn1 long-hold (silk SW3 on Rev 3/A, SW6 on Rev 2); GPIO%d wakes",
             s_wake_btn);
    return ESP_OK;
}

// ==========================================================
// Console command
// ==========================================================

static int cmd_sleep(int argc, char **argv)
{
    if (!s_available) {
        printf("Sleep not supported on this board revision.\n");
        return 1;
    }
    printf("Entering sleep mode. Wake: press the wake button (GPIO%d).\n"
           "USB attached: console stays connected (idle mode).\n", s_wake_btn);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));    // let the TinyUSB CDC console flush
    return power_sleep_enter() == ESP_OK ? 0 : 1;
}

void register_sleep_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command = "sleep",
        .help    = "Enter light sleep (5s status blink; wake button resumes)",
        .func    = cmd_sleep,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
