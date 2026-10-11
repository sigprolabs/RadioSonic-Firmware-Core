// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// Shared HAL implementation for Rev 2, Rev 3 and Rev A (production) boards —
// the pins come from hal.h, which resolves to hal_rev2.h, hal_rev3.h or
// hal_reva.h per the Kconfig board choice; the code is identical for all
// three. Log strings therefore stay revision-neutral.
#include "hal.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static const char *TAG = "hal";

// SPI-bus bring-up. A failure here is unrecoverable at boot (no codec, no
// audio), hence ESP_ERROR_CHECK; the esp_err_t return exists only to keep the
// HAL signature uniform across revisions.
esp_err_t hal_spi_init(void) {
    gpio_set_direction(AIC_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(AIC_CS_PIN, 1);

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
    ESP_LOGI(TAG, "SPI bus init complete.");
    return ESP_OK;
}

esp_err_t hal_mount_sdcard(sdmmc_card_t **card_out) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 10,
        .allocation_unit_size   = 64 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 5)
    // Transfers to/from non-DMA buffers (PSRAM) bounce one 512 B block at a
    // time, as on every earlier IDF. v5.5.5 defaults to 16-block chunks,
    // which allocates an 8 KB internal DMA buffer per transfer: unbudgeted
    // internal RAM (see the ledger in docs/ARCHITECTURE.md). Raise only
    // together with a heap measurement.
    host.unaligned_multi_block_rw_max_chunk_size = 1;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width  = 4;
    slot_config.clk    = SD_CLK_PIN;
    slot_config.cmd    = SD_CMD_PIN;
    slot_config.d0     = SD_D0_PIN;
    slot_config.d1     = SD_D1_PIN;
    slot_config.d2     = SD_D2_PIN;
    slot_config.d3     = SD_D3_PIN;
    slot_config.cd     = SD_CD_PIN;
    slot_config.wp     = SDMMC_SLOT_NO_WP;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting SD card (SDMMC 4-bit)...");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, card_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC mount failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void hal_buttons_init(void) {
    const gpio_num_t btns[] = {
        BUTTON_GPIO,                                       // SW2 (BOOT) — shift
        BTN_EXT1_GPIO, BTN_PCM_DIN_GPIO, BTN_PCM_DOUT_GPIO,
        BTN_PCM_FS_GPIO, BTN_PCM_CLK_GPIO
    };
    gpio_config_t cfg = {
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < (int)(sizeof(btns) / sizeof(btns[0])); i++) {
        cfg.pin_bit_mask = (1ULL << btns[i]);
        gpio_config(&cfg);
    }
}

bool hal_sleep_buttons(gpio_num_t *wake) {
    *wake = BTN_EXT1_GPIO;   // btn1 (silk SW6 Rev 2, SW3 Rev 3/A) — long-hold sleeps, press wakes
    return true;
}

// Only Rev 3 and Rev A define POT_ADC_GPIO (the wiper on IO1). Rev 2's ADC_IN
// net sits on IO42, which has no ADC on the S3.
bool hal_pot(gpio_num_t *gpio) {
#ifdef POT_ADC_GPIO
    if (gpio) *gpio = POT_ADC_GPIO;
    return true;
#else
    (void)gpio;
    return false;
#endif
}

// Positional map (see hal_panel_buttons_t). Silk names differ per rev:
//   position:      btn1   btn2   btn3   btn4   btn5
//   GPIO:          8      41     40     39     38
//   Rev 2 silk:    SW6    SW3    SW7    SW4    SW5
//   Rev 3/A silk:  SW3    SW4    SW5    SW6    SW7   (renumbered left-to-right)
bool hal_panel_buttons(hal_panel_buttons_t *map) {
    map->shift = BUTTON_GPIO;        // BOOT (GPIO0; Rev 2 silk SW2)
    map->btn1  = BTN_EXT1_GPIO;      // GPIO8
    map->btn2  = BTN_PCM_CLK_GPIO;   // GPIO41
    map->btn3  = BTN_PCM_FS_GPIO;    // GPIO40
    map->btn4  = BTN_PCM_DOUT_GPIO;  // GPIO39
    map->btn5  = BTN_PCM_DIN_GPIO;   // GPIO38
    return true;
}

// (The LED status/needle POLICY lives in main/ui/led_status.c; the strip
// DRIVER in rgb_leds.c self-stubs on boards without a strip. Nothing
// LED-related remains in the HAL — the board layer stays app-state-free.)
