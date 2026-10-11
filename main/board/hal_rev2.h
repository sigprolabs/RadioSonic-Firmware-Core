// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include "driver/gpio.h"
#include "driver/spi_master.h"

// ==========================================================
// Rev 2 Pin Definitions
// ==========================================================
//
// PCB errata RETRACTED 2026-07-08: HPL/HPR and LOL/LOR are wired STRAIGHT
// (schematic + DRC verified; the L/R swap also reproduces on Rev 1, so it was
// never board layout). The L/R investigation lives in issue #1; if firmware
// compensation is needed it belongs in audio_dsp.c / audio_codec.c with a
// measurement citation — not blamed on this board.

#define BOARD_NAME          "Rev 2"

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
// Jamie 2026-09-05: Rev 2 = 28 mm (Rev 3 / Rev A = 30 mm).
#define MIC_SPACING_M       0.028f

// Pushbuttons (active low — internal pullup enabled)
#define BTN_EXT1_GPIO       GPIO_NUM_8   // EXT_GPIO_1
#define BTN_PCM_DIN_GPIO    GPIO_NUM_38  // PCM_DIN / RPi pin
#define BTN_PCM_DOUT_GPIO   GPIO_NUM_39  // PCM_DOUT / RPi pin
#define BTN_PCM_FS_GPIO     GPIO_NUM_40  // PCM_FS / RPi pin
#define BTN_PCM_CLK_GPIO    GPIO_NUM_41  // PCM_CLK / RPi pin

// User Interface
#define BLINK_GPIO          GPIO_NUM_2
#define BUTTON_GPIO         GPIO_NUM_0   // Boot Button (Active Low)
