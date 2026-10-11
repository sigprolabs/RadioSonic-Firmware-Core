// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#include "hal.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static const char *TAG = "hal_rev1";

esp_err_t hal_spi_init(void) {
    gpio_set_direction(AIC_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(AIC_CS_PIN, 1);

    // Rev 1: SD shares the SPI bus — hold CS high before any transactions
    gpio_set_direction(SD_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SD_CS_PIN, 1);

    gpio_reset_pin(SPI_MISO_PIN);
    gpio_set_direction(SPI_MISO_PIN, GPIO_MODE_INPUT);

    gpio_set_pull_mode(SPI_MISO_PIN, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SPI_MOSI_PIN, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SPI_SCLK_PIN, GPIO_PULLUP_ONLY);

    spi_bus_config_t buscfg = {
        .mosi_io_num     = SPI_MOSI_PIN,
        .miso_io_num     = SPI_MISO_PIN,
        .sclk_io_num     = SPI_SCLK_PIN,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 131072,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_USED, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI bus init complete (Rev 1).");
    return ESP_OK;
}

esp_err_t hal_mount_sdcard(sdmmc_card_t **card_out) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 10,
        .allocation_unit_size   = 64 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot        = SPI_HOST_USED;
    host.max_freq_khz = 20000;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 5)
    // Single-block bounce for non-DMA buffers; see hal_rev2.c.
    host.unaligned_multi_block_rw_max_chunk_size = 1;
#endif

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = SPI_HOST_USED;
    slot_config.gpio_cd = GPIO_NUM_NC;

    ESP_LOGI(TAG, "Mounting SD card (SDSPI, Rev 1)...");
    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, card_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDSPI mount failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void hal_buttons_init(void)      { /* Rev 1: no extra buttons */ }

bool hal_sleep_buttons(gpio_num_t *wake) {
    (void)wake;
    return false;  // Rev 1: no buttons — sleep feature unavailable
}

bool hal_panel_buttons(hal_panel_buttons_t *map) {
    (void)map;
    return false;  // Rev 1: no buttons — panel controls unavailable
}

bool hal_pot(gpio_num_t *gpio) {
    (void)gpio;
    return false;  // Rev 1: ADC_IN is on IO5 but no pot is fitted
}
// (No LED functions here: the strip driver rgb_leds.c self-stubs on boards
// whose pin header defines no RGB_LED_PIN — Rev 1 included.)
