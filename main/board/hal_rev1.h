// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include "driver/gpio.h"
#include "driver/spi_master.h"

// ==========================================================
// Rev 1 Pin Definitions
// ==========================================================

#define BOARD_NAME          "Rev 1"

// Shared SPI Bus (Codec Control + SD Card on same bus)
#define SPI_HOST_USED       SPI2_HOST
#define SPI_SCLK_PIN        GPIO_NUM_10
#define SPI_MISO_PIN        GPIO_NUM_3
#define SPI_MOSI_PIN        GPIO_NUM_46

// Chip Selects
#define AIC_CS_PIN          GPIO_NUM_9   // Codec CS
#define CODEC_1_CS_PIN      GPIO_NUM_42  // Expansion codec 1 CS (mic expansion connector; confirmed 2026-07-17)
#define CODEC_2_CS_PIN      GPIO_NUM_45  // Expansion codec 2 CS (confirmed 2026-07-17)
#define SD_CS_PIN           GPIO_NUM_8   // SD Card CS (SDSPI)
#define SD_CD_PIN           GPIO_NUM_17  // SD Card Detect (present on the PCB, unused by firmware — mount passes GPIO_NUM_NC)

// I2S Audio Bus
#define I2S_MCLK_PIN        GPIO_NUM_21
#define I2S_BCLK_PIN        GPIO_NUM_14
#define I2S_LRC_PIN         GPIO_NUM_13
#define I2S_DOUT_PIN        GPIO_NUM_12  // ESP TX -> Codec DIN
#define I2S_DIN_PIN         GPIO_NUM_11  // ESP RX <- Codec DOUT

// On-board electret mic pair, center-to-center spacing in meters (board
// geometry for effects that work from the mics' phase difference).
// UNMEASURED on Rev 1: Rev 2's 28 mm is assumed. Measure before relying on it.
#define MIC_SPACING_M       0.028f

// User Interface
#define BLINK_GPIO          GPIO_NUM_4
#define BUTTON_GPIO         GPIO_NUM_0   // Boot Button (Active Low)
