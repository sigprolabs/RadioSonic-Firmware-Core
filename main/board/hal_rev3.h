// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include "driver/gpio.h"
#include "driver/spi_master.h"

// ==========================================================
// Rev 3 Pin Definitions (board SPL-RS-01-03)
// ==========================================================
//
// Identical to Rev 2 except (this delta list is the drift guard for public
// readers — keep it complete):
//   - BLINK_GPIO moved GPIO2 -> GPIO45 (GPIO2 is now I2C SDA — never drive it)
//   - I2C added to the J6 Pi header: GPIO2 = SDA, GPIO42 = SCL
//   - Analog pot wiper on GPIO1/ADC1_CH0 (enables the whole control_pot
//     feature — absent on Rev 2, whose ADC_IN lands on no-ADC IO42)
//   - Silkscreen button numbering shifted (GPIOs unchanged; buttons are
//     addressed positionally, btn1..btn5 — see hal_panel_buttons in hal_rev2.c)
// Source: KiCad netlist of Rev 3 as-built (hardware repo a1fcf398, "rev 3 fab
// output", 2026-07-15), re-exported 2026-09-28. The HAL implementation is
// shared with Rev 2 (hal_rev2.c). Production "Rev A" boards (SPL-RS-01-A) have
// the same ESP32 pinout but their own header: hal_reva.h.

#define BOARD_NAME          "Rev 3 (SPL-RS-01-03)"

// Shared SPI Bus (Codec Control only — SD is SDMMC 4-bit)
#define SPI_HOST_USED       SPI2_HOST
#define SPI_SCLK_PIN        GPIO_NUM_10
#define SPI_MISO_PIN        GPIO_NUM_3
#define SPI_MOSI_PIN        GPIO_NUM_46

// Codec Chip Selects
#define AIC_CS_PIN          GPIO_NUM_9   // Primary codec CS
#define CODEC_1_CS_PIN      GPIO_NUM_48  // Expansion codec 1 CS (TDM; CODEC_EXP1 in audio_codec.c — bench-verified 2026-07-27)
#define CODEC_2_CS_PIN      GPIO_NUM_47  // Expansion codec 2 CS (TDM; CODEC_EXP2 in audio_codec.c)

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
// Jamie 2026-09-05: Rev 3 / production Rev A = 30 mm (Rev 2 was 28 mm).
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

// I2C (new on Rev 3) to the J6 Raspberry Pi header only (J6.3 = SDA, J6.5 =
// SCL). No firmware driver yet — defined so nothing else claims these pins.
// The CAT24C32 (U4) is not on this bus: it is the Pi HAT ID EEPROM, on
// J6.27/28 (ID_SD/ID_SC), reachable only from the Pi.
#define I2C_SCL_PIN         GPIO_NUM_42
#define I2C_SDA_PIN         GPIO_NUM_2

// Analog control pot — wiper (R13) on IO1 = ADC1_CH0, as drawn in the Rev 3
// netlist. (Earlier notes here called this an "IO1 rework" and flagged a clash
// with I2C SCL; both came from a stale Pinouts.csv column.)
// ADC1 is a hard requirement, not a preference: ADC2 shares the SAR block with
// the WiFi PHY and reads fail while WiFi is up (issue #7 notes in cli_console.c).
#define POT_ADC_GPIO        GPIO_NUM_1

// Pushbuttons (active low — internal pullup enabled). GPIOs match Rev 2;
// Rev 3 relabeled the panel row so silk reads SW3..SW7 LEFT TO RIGHT (the
// same physical row read 6-3-7-4-5 in Rev 2 refdes). hal_panel_buttons_t
// (hal.h) addresses them positionally, btn1..btn5, so silk never matters:
#define BTN_EXT1_GPIO       GPIO_NUM_8   // EXT_GPIO_1 — silk SW3 (Rev 2: SW6)
#define BTN_PCM_DIN_GPIO    GPIO_NUM_38  // PCM_DIN    — silk SW7 (Rev 2: SW5)
#define BTN_PCM_DOUT_GPIO   GPIO_NUM_39  // PCM_DOUT   — silk SW6 (Rev 2: SW4)
#define BTN_PCM_FS_GPIO     GPIO_NUM_40  // PCM_FS     — silk SW5 (Rev 2: SW7)
#define BTN_PCM_CLK_GPIO    GPIO_NUM_41  // PCM_CLK    — silk SW4 (Rev 2: SW3)

// User Interface
// BLINK_GPIO is the VDD_SPI-voltage strapping pin: safe as an output after
// boot; the LED wiring (IO45 -> R10 -> LED -> GND, active-high) leaves the
// strap low at reset.
#define BLINK_GPIO          GPIO_NUM_45
#define BUTTON_GPIO         GPIO_NUM_0   // Boot Button SW2 (Active Low)
