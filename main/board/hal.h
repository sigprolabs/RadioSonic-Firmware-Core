// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"

// Board revision selector — set via idf.py menuconfig → RadioSonic Board Configuration
#if defined(CONFIG_BOARD_REVISION_REV1)
  #include "hal_rev1.h"
#elif defined(CONFIG_BOARD_REVISION_REV2)
  #include "hal_rev2.h"
#elif defined(CONFIG_BOARD_REVISION_REV3)
  #include "hal_rev3.h"
#elif defined(CONFIG_BOARD_REVISION_REVA)
  #include "hal_reva.h"
#else
  #error "No board revision selected. Run idf.py menuconfig and set RadioSonic Board Configuration."
#endif

// ==========================================================
// HAL API — each hal_rev*.c provides these implementations
// ==========================================================
// Rev 3 and Rev A (production) share hal_rev2.c and behave as Rev 2
// everywhere below unless a row says otherwise; only the pin header differs
// (see the delta lists in hal_rev3.h and hal_reva.h). Every pin header also
// defines BOARD_NAME, which `ver` prints so a wrong-revision build is visible
// on the board itself (the choice lives only in the untracked sdkconfig).

// Initialize the shared SPI bus and codec CS pin.
// Rev 1 also pulls the SD CS pin high before any SPI traffic.
esp_err_t hal_spi_init(void);

// Mount the SD card and return the card handle.
// Rev 1: SDSPI (20 MHz, SPI2)
// Rev 2: SDMMC 4-bit (40 MHz)
esp_err_t hal_mount_sdcard(sdmmc_card_t **card_out);

// Configure board-specific pushbuttons with internal pull-ups.
// Rev 1: no-op  |  Rev 2: GPIO8, 38-41
void hal_buttons_init(void);

// LEDs are NOT part of this API: the strip driver (board/rgb_leds.c) self-
// stubs on boards whose pin header defines no RGB_LED_PIN, and the
// status/needle POLICY lives in main/ui/led_status.c — so the HAL carries no
// app-state dependencies.

// Sleep wake button (btn1 — the only button that can wake from light sleep).
// Returns true on boards that support sleep. Rev 1: returns false.
bool hal_sleep_buttons(gpio_num_t *wake);

// Front-panel button map (active low, internal pull-ups). The shift (hold)
// modifier gives each top-row button two functions — see control_buttons.c
// for the assignment table (issue #8).
// Fields are named POSITIONALLY (left to right across the panel), not by
// silkscreen refdes: the silk was renumbered between Rev 2 and Rev 3, so a
// refdes name would point at the wrong printed label on one board or the
// other. Each rev header's comments map positions to that rev's silk.
// Rev 1: returns false (no panel buttons).
typedef struct {
    gpio_num_t shift;                       // BOOT button — hold for shifted layer
    gpio_num_t btn1, btn2, btn3, btn4, btn5; // top row, left to right
} hal_panel_buttons_t;
bool hal_panel_buttons(hal_panel_buttons_t *map);

// Analog control potentiometer (wiper -> ADC). Returns false on boards without
// one, which no-ops the whole pot feature (control_pot.c).
// The pin MUST be an ADC1 channel (GPIO1-10 on the ESP32-S3): ADC2 shares the
// SAR hardware with the WiFi PHY and stops reading once WiFi is up.
//   Rev 1: false (no pot fitted — its ADC_IN net does land on an ADC1 pin).
//   Rev 2: false (ADC_IN is routed to IO42, which has no ADC on the S3).
//   Rev 3 / Rev A: true, GPIO1 (see POT_ADC_GPIO in hal_rev3.h / hal_reva.h).
bool hal_pot(gpio_num_t *gpio);
