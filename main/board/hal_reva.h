// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include "driver/gpio.h"
#include "driver/spi_master.h"

// ==========================================================
// Rev A Pin Definitions (board SPL-RS-01-A — the production board)
// ==========================================================
//
// Rev A is the production respin of Rev 3 (SPL-RS-01-03). Its ESP32-S3 pinout
// is IDENTICAL to Rev 3's: every U3 pin lands on the same net with the same
// loads. Source: KiCad netlists exported from the hardware repo and diffed
// 2026-09-28 — Rev 3 as-built a1fcf398 ("rev 3 fab output") vs Rev A as-built
// 0b041fd (its commit message reads "Rev 3 As Built"; that is a typo — it is
// Rev A). The HAL implementation is shared with Rev 2/3 (hal_rev2.c).
//
// What Rev A changed is off the ESP32's pins. Keep this list complete — it is
// how a reader decides whether a Rev 3 observation applies to Rev A:
//   - R34 / R33 (new): 10 k from the codec's AVDD / DVDD to GND — the
//     hardware fix for the codec wedging after a ~5 s power interruption
//     (hardware issue #36; codec RESET is only a pull-up). Side effect for
//     firmware: until codec init enables the AVDD LDO (P1 R2), AVDD is fed only
//     by the codec's weak internal link, which R34 loads (≈0.86 V estimated,
//     never scoped). Rev A shows the LINE IN no-input fault (audio_codec.c)
//     more often than Rev 3; whether R34 is why is untested — the fault also
//     occurs on bare Rev 3. Enabling the LDO first at boot was proposed, not
//     implemented.
//   - Line-in jumper headers J7, J8, J10, J11 removed: the line input is
//     hard-wired pseudo-differential, so `input diff` is the only valid line
//     mode. (Rev 3 was jumper-configurable; all of its DVT ran in diff.)
//   - SW8 (new): slide power switch between the charger output and +5VB. It
//     cuts USB and battery power alike; no GPIO sees it.
//   - Charger (U5) only, no firmware effect: R35 69.8 k on TMR (safety timers
//     ~56 min / 9.3 h), R26 3.9 k -> 1.5 k on ISET (~228 -> ~593 mA with a
//     battery fitted). Part swaps: twelve 1 uF caps -> CGA0402X5R105M160GT,
//     R12/R25/R29 0.1% -> 1%.

#define BOARD_NAME          "Rev A (SPL-RS-01-A)"

// Shared SPI Bus (Codec Control only — SD is SDMMC 4-bit)
#define SPI_HOST_USED       SPI2_HOST
#define SPI_SCLK_PIN        GPIO_NUM_10
#define SPI_MISO_PIN        GPIO_NUM_3
#define SPI_MOSI_PIN        GPIO_NUM_46

// Codec Chip Selects
#define AIC_CS_PIN          GPIO_NUM_9   // Primary codec CS
#define CODEC_1_CS_PIN      GPIO_NUM_48  // Expansion codec 1 CS (J5; CODEC_EXP1 in audio_codec.c)
#define CODEC_2_CS_PIN      GPIO_NUM_47  // Expansion codec 2 CS (J5; CODEC_EXP2 in audio_codec.c)

// SD Card — SDMMC 4-bit mode
#define SD_CD_PIN           GPIO_NUM_4   // Card Detect (active low)
#define SD_CMD_PIN          GPIO_NUM_15
#define SD_CLK_PIN          GPIO_NUM_7
#define SD_D0_PIN           GPIO_NUM_6
#define SD_D1_PIN           GPIO_NUM_5
#define SD_D2_PIN           GPIO_NUM_17
#define SD_D3_PIN           GPIO_NUM_16

// I2S Audio Bus
#define I2S_MCLK_PIN        GPIO_NUM_21
#define I2S_BCLK_PIN        GPIO_NUM_14
#define I2S_LRC_PIN         GPIO_NUM_13
#define I2S_DOUT_PIN        GPIO_NUM_12  // ESP TX -> Codec DIN
#define I2S_DIN_PIN         GPIO_NUM_11  // ESP RX <- Codec DOUT

// RGB LED Strip (SK6805-EC15, NeoPixel-compatible, 4 LEDs chained)
#define RGB_LED_PIN         GPIO_NUM_18
#define RGB_LED_COUNT       4

// On-board electret mic pair, center-to-center spacing in meters (board
// geometry for effects that work from the mics' phase difference). From layout, per
// Jamie 2026-09-05: Rev 3 / Rev A = 30 mm (Rev 2 was 28 mm).
#define MIC_SPACING_M       0.030f

// LINE IN jack wiring: the PJ-3200 footprint puts the plug TIP (the left
// signal) on the pad the schematic calls R, so tip -> IN3R -> the codec's
// RIGHT channel and ring -> IN2L -> LEFT (measured on Rev A and in Rev 3 DVT).
// The output jacks have the same footprint; P0 R63 (audio_codec.c) compensates
// there. For the input the codec can't swap its ADCs without inverting one
// channel, so rx_fill swaps the main codec's pair back in line-in mode
// (firmware issue #20; the footprint itself is hardware issue #38). Mic
// mode is wired straight.
#define LINE_IN_LR_SWAPPED  1

// I2C to the J6 Raspberry Pi header only (J6.3 = SDA, J6.5 = SCL). No firmware
// driver yet — defined so nothing else claims these pins. The CAT24C32 (U4)
// is NOT on this bus: it is the Pi HAT ID EEPROM, on J6.27/28 (ID_SD/ID_SC),
// and only the Pi can reach it.
#define I2C_SCL_PIN         GPIO_NUM_42
#define I2C_SDA_PIN         GPIO_NUM_2

// Analog control pot — wiper (R13) on IO1 = ADC1_CH0, as drawn. ADC1 is a hard
// requirement: ADC2 shares the SAR block with the WiFi PHY and reads fail
// while WiFi is up (issue #7 notes in cli_console.c).
#define POT_ADC_GPIO        GPIO_NUM_1

// Pushbuttons (active low — internal pullup enabled). Silk reads SW3..SW7
// LEFT TO RIGHT; hal_panel_buttons() addresses them positionally (btn1..btn5).
// SW1 = EN (reset), SW2 = BOOT, SW8 = power switch — none of those three is a
// panel button.
#define BTN_EXT1_GPIO       GPIO_NUM_8   // EXT_GPIO_1 — silk SW3
#define BTN_PCM_CLK_GPIO    GPIO_NUM_41  // PCM_CLK    — silk SW4
#define BTN_PCM_FS_GPIO     GPIO_NUM_40  // PCM_FS     — silk SW5
#define BTN_PCM_DOUT_GPIO   GPIO_NUM_39  // PCM_DOUT   — silk SW6
#define BTN_PCM_DIN_GPIO    GPIO_NUM_38  // PCM_DIN    — silk SW7

// User Interface
// BLINK_GPIO is the VDD_SPI-voltage strapping pin: safe as an output after
// boot; the LED wiring (IO45 -> R10 -> LED -> GND, active-high) leaves the
// strap low at reset.
#define BLINK_GPIO          GPIO_NUM_45
#define BUTTON_GPIO         GPIO_NUM_0   // Boot Button SW2 (Active Low)
