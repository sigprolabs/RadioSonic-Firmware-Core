// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include "esp_err.h"

// Light-sleep power mode (issue #2).
//
// Entry:  long-hold the feature button (btn1, ~1.75 s — detected in
//         control_buttons.c), or run the `sleep` console command.
// Asleep: light sleep with all state preserved; wakes every 5 s to blink
//         BLINK_GPIO for ~100 ms, then sleeps again.
// Wake:   press the wake button (btn1 again).
//
// power_sleep_enter() returns ESP_ERR_NOT_SUPPORTED on boards without a wake
// button (Rev 1); power_sleep_init() still returns ESP_OK there.

// Resolve the wake pin from the HAL. Starts no task — the hold detection
// lives in control_buttons.c.
esp_err_t power_sleep_init(void);

// Register the `sleep` console command.
void register_sleep_cmd(void);

// Blocking: quiesce audio/WiFi/codec, run the sleep/blink loop until the
// wake button is pressed, then restore everything. Safe to call from the
// console REPL task or the monitor task.
esp_err_t power_sleep_enter(void);
