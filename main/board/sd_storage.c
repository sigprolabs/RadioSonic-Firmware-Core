// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#include "sd_storage.h"
#include "common.h"
#include "audio_store.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "sd";

// Format the SD card (FAT, erases everything) and remount it.
// Requires the literal argument "yes" so a stray "sd_format" can't wipe a card.
static int cmd_sd_format(int argc, char **argv)
{
    if (g_state.sd_card == NULL) {
        printf("No SD card mounted.\n");
        return 1;
    }
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        printf("This ERASES ALL DATA on the SD card.\n"
               "To confirm, run:  sd_format yes\n");
        return 1;
    }

    // A format while the recorder/player holds open files would corrupt the
    // new filesystem — stop them first.
    if (audio_store_is_recording()) audio_store_record_stop();
    if (audio_store_is_playing())   audio_store_play_stop();

    printf("Formatting SD card (this can take a while on large cards)...\n");
    fflush(stdout);

    esp_err_t err = esp_vfs_fat_sdcard_format(MOUNT_POINT, g_state.sd_card);
    if (err != ESP_OK) {
        printf("Format failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &free_b) == ESP_OK) {
        printf("Format complete. %llu MB free of %llu MB.\n",
               (unsigned long long)(free_b / (1024 * 1024)),
               (unsigned long long)(total  / (1024 * 1024)));
    } else {
        printf("Format complete.\n");
    }
    return 0;
}

void register_sd_format_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command = "sd_format",
        .help    = "Format the SD card (erases all data) — requires 'sd_format yes'",
        .func    = cmd_sd_format,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

// Boot-time mount logs print before the TinyUSB console exists and are lost —
// keep the result queryable (`status`, sd_remount) after the fact.
static esp_err_t s_last_mount_err = ESP_ERR_INVALID_STATE;  // "never attempted"

esp_err_t sd_last_mount_err(void) { return s_last_mount_err; }

esp_err_t mount_sdcard(void) {
    esp_err_t ret = hal_mount_sdcard(&g_state.sd_card);
    s_last_mount_err = ret;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Mounted at %s", MOUNT_POINT);
        sdmmc_card_print_info(stdout, g_state.sd_card);
        ESP_LOGI(TAG, "Card: max=%u kHz, actual=%d kHz",
                 (unsigned)g_state.sd_card->max_freq_khz,
                 g_state.sd_card->real_freq_khz);
    } else {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        g_state.sd_card = NULL;
    }
    return ret;
}

// Retry the mount interactively — recovery attempt AND diagnostic: the mount
// runs the full card init sequence (CMD0/CMD8/ACMD41...), so its error names
// which stage a wedged card dies at, on a console that actually exists.
static int cmd_sd_remount(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (g_state.sd_card != NULL) {
        printf("SD already mounted:\n");
        sdmmc_card_print_info(stdout, g_state.sd_card);
        return 0;
    }
    printf("Last mount result: %s\nAttempting remount...\n",
           esp_err_to_name(s_last_mount_err));
    fflush(stdout);
    esp_err_t ret = mount_sdcard();
    if (ret == ESP_OK) {
        printf("Remount OK — card recovered without power cycle.\n");
        return 0;
    }
    printf("Remount failed: %s\n"
           "(SDMMC card power is hardwired — if re-init can't recover it, only a\n"
           " physical power cycle resets the card's internal controller.)\n",
           esp_err_to_name(ret));
    return 1;
}

void register_sd_remount_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command = "sd_remount",
        .help    = "Retry SD mount after a boot-time failure or runtime wedge",
        .func    = cmd_sd_remount,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
