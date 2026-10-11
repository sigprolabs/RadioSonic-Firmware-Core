// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#include "rgb_leds.h"
#include "hal.h"                 // RGB_LED_PIN / RGB_LED_COUNT (per-rev header)

// Self-stubbing driver: boards whose pin header defines no RGB_LED_PIN
// (Rev 1) compile the empty stubs at the bottom instead — callers never need
// a board check, and the status policy in main/ui/led_status.c just no-ops.
#ifdef RGB_LED_PIN

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// SK6805-EC15 uses GRB byte order, WS2812-compatible protocol.
// RMT clock: 10 MHz → 1 tick = 100 ns
#define RMT_RESOLUTION_HZ   10000000

// SK6805-EC15 timing (ticks at 10 MHz):
//   T0H = 300 ns → 3 ticks,  T0L = 900 ns → 9 ticks
//   T1H = 600 ns → 6 ticks,  T1L = 600 ns → 6 ticks
#define T0H  3
#define T0L  9
#define T1H  6
#define T1L  6

static rmt_channel_handle_t s_led_chan    = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;

// Framebuffer in GRB order (strip native), updated by rgb_leds_set() and
// pushed by rgb_leds_refresh().
static uint8_t s_fb[RGB_LED_COUNT * 3];

void rgb_leds_init(void) {
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num            = RGB_LED_PIN,
        .clk_src             = RMT_CLK_SRC_DEFAULT,
        .resolution_hz       = RMT_RESOLUTION_HZ,
        .mem_block_symbols   = 64,
        .trans_queue_depth   = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_led_chan));

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .duration0 = T0H, .level0 = 1,
                  .duration1 = T0L, .level1 = 0 },
        .bit1 = { .duration0 = T1H, .level0 = 1,
                  .duration1 = T1L, .level1 = 0 },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &s_led_encoder));

    ESP_ERROR_CHECK(rmt_enable(s_led_chan));
}

// Write a GRB buffer (RGB_LED_COUNT * 3 bytes) to the strip.
static void leds_write(const uint8_t *grb, size_t len) {
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(s_led_chan, s_led_encoder, grb, len, &tx_cfg));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_led_chan, 100));
}

// Scale a byte by a 0-255 brightness factor.
static inline uint8_t scale(uint8_t val, uint8_t brightness) {
    return (uint8_t)(((uint16_t)val * brightness) >> 8);
}

void rgb_leds_startup_anim(void) {
    // Each LED's full-brightness GRB value (strip expects G, R, B)
    static const uint8_t colors[RGB_LED_COUNT][3] = {
        { 0,   255, 0   },  // LED 0: red   → GRB = {0, 255, 0}
        { 255, 0,   0   },  // LED 1: green → GRB = {255, 0, 0}
        { 0,   0,   255 },  // LED 2: blue  → GRB = {0, 0, 255}
        { 255, 255, 255 },  // LED 3: white → GRB = {255, 255, 255}
    };

    const int steps    = 20;
    const int step_ms  = 25;

    uint8_t grb[RGB_LED_COUNT * 3];

    for (int led = 0; led < RGB_LED_COUNT; led++) {
        // Fade in
        for (int s = 0; s <= steps; s++) {
            uint8_t b = (uint8_t)((s * 255) / steps);
            memset(grb, 0, sizeof(grb));
            grb[led * 3 + 0] = scale(colors[led][0], b);
            grb[led * 3 + 1] = scale(colors[led][1], b);
            grb[led * 3 + 2] = scale(colors[led][2], b);
            leds_write(grb, sizeof(grb));
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
        // Fade out
        for (int s = steps; s >= 0; s--) {
            uint8_t b = (uint8_t)((s * 255) / steps);
            memset(grb, 0, sizeof(grb));
            grb[led * 3 + 0] = scale(colors[led][0], b);
            grb[led * 3 + 1] = scale(colors[led][1], b);
            grb[led * 3 + 2] = scale(colors[led][2], b);
            leds_write(grb, sizeof(grb));
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    }

    // All off
    memset(grb, 0, sizeof(grb));
    leds_write(grb, sizeof(grb));
}

void rgb_leds_all_off(void) {
    if (!s_led_chan) return;  // ordering guard: callable before led_task runs rgb_leds_init (e.g. an early all_off from power_sleep)
    memset(s_fb, 0, sizeof(s_fb));
    uint8_t grb[RGB_LED_COUNT * 3] = { 0 };
    leds_write(grb, sizeof(grb));
}

void rgb_leds_set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx >= RGB_LED_COUNT) return;
    // Strip byte order is GRB.
    s_fb[idx * 3 + 0] = g;
    s_fb[idx * 3 + 1] = r;
    s_fb[idx * 3 + 2] = b;
}

void rgb_leds_refresh(void) {
    if (!s_led_chan) return;  // ordering guard: callable before led_task runs rgb_leds_init (e.g. an early all_off from power_sleep)
    leds_write(s_fb, sizeof(s_fb));
}

#else  // no RGB_LED_PIN on this board revision — every call is a no-op

void rgb_leds_init(void)         { }
void rgb_leds_startup_anim(void) { }
void rgb_leds_all_off(void)      { }
void rgb_leds_set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b)
                                 { (void)idx; (void)r; (void)g; (void)b; }
void rgb_leds_refresh(void)      { }

#endif
