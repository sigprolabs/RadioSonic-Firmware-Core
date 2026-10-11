// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once
#include "esp_err.h"

#define MOUNT_POINT "/sdcard"

// Mounts the SD Card to the VFS. Returns ESP_OK on success.
esp_err_t mount_sdcard(void);

// Last mount_sdcard() result (boot or sd_remount) — shown by `status` so a
// boot-time mount failure isn't lost with the pre-USB console output.
esp_err_t sd_last_mount_err(void);

// Register the `sd_format` console command (formats the card; needs "yes" arg).
void register_sd_format_cmd(void);

// Register `sd_remount`: retry mounting a card that failed at boot (or wedged
// at runtime — SDMMC 2026-07-08 incident: HLE + CMD timeouts, state survives
// warm resets because the card keeps 3.3 V). Prints the exact failure stage.
void register_sd_remount_cmd(void);