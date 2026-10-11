# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
# Sources are grouped by responsibility:
#   audio/  — signal path: codec, DSP framework + effect gallery, recorder, WAV
#   board/  — hardware-specific: HAL (rev1/rev2/rev3/revA), LEDs, SD mount, sleep/power
#   ui/     — control surfaces: CLI console, panel buttons, web server, xfer
# Board-specific HAL source is selected by the Kconfig revision choice; core
# app code (incl. dsp_effects.c and the board-agnostic control_buttons.c,
# which no-ops on boards without buttons) is always compiled.
# Rev 3 and Rev A share the Rev 2 HAL implementation — hal_rev2.c takes every
# pin from hal.h, which resolves to hal_rev3.h / hal_reva.h (see those headers'
# delta lists: BLINK_GPIO, I2C pins, and the pot differ from Rev 2).
if(CONFIG_BOARD_REVISION_REV2 OR CONFIG_BOARD_REVISION_REV3 OR CONFIG_BOARD_REVISION_REVA)
    set(HAL_SRCS "board/hal_rev2.c")
else()
    set(HAL_SRCS "board/hal_rev1.c")
endif()

idf_component_register(
    SRCS
        "radiosonic_main.c"
        "audio/audio_codec.c"
        "audio/input_guard.c"
        "audio/audio_dsp.c"
        "audio/dsp_effects.c"
        "audio/channel_fx.c"
        "audio/audio_store.c"
        "board/sd_storage.c"
        "board/power_sleep.c"
        "board/rgb_leds.c"
        "ui/cli_console.c"
        "ui/led_status.c"
        "ui/control_buttons.c"
        "ui/control_pot.c"
        "ui/serial_xfer.c"
        "ui/settings_store.c"
        "ui/web_server.c"
        "ui/wifi_manager.c"
        ${HAL_SRCS}
    INCLUDE_DIRS "." "audio" "board" "ui"

    # The web UI is real HTML/CSS/JS embedded at build time (TXTFILES adds the
    # NUL terminator h_root relies on). Edit ui/web_page.html directly.
    # channel_panel.html is the channel effect's web card (its .web_panel).
    EMBED_TXTFILES "ui/web_page.html" "audio/channel_panel.html"

    REQUIRES
        console                     # For REPL/CLI features
        driver                      # General hardware drivers
        esp_driver_i2s              # Audio I/O
        esp_driver_spi              # Codec Control
        esp_driver_sdmmc            # SD Card (SDMMC 4-bit, Rev 2)
        esp_driver_sdspi            # SD Card (SDSPI, Rev 1)
        esp_driver_gpio             # Pin control
        esp_driver_rmt              # RGB LED strip (SK6805, Rev 2)
        esp_tinyusb                 # Native USB: CDC console + fast transfer (+ optional MSC)
        fatfs                       # File System for SD Card
        sdmmc                       # SD Card Protocol
        esp_timer
        esp_adc                     # analog pot (control_pot.c) + `adc` CLI test command
        vfs
        spi_flash                   # Raw partition read/write (audio_store flash backend)
        esp_wifi
        esp_netif
        esp_http_server
        nvs_flash
)

# The DSP hot path runs 48000x/s on core 1; IDF's default -Os leaves real
# throughput on the table there. Build just the signal-path files at -O2.
# (Measured impact: issue #10.)
set_source_files_properties(
    "audio/audio_dsp.c" "audio/dsp_effects.c" "audio/channel_fx.c"
    PROPERTIES COMPILE_OPTIONS "-O2"
)

# Diagnostic bench builds only: `idf.py -B build_diag -DCODEC_DIAG_ADOPT=1 build`
# adopts a running codec at boot instead of soft-resetting it and enables
# `codec wr` (see audio_codec.h). Keep it in a separate build dir so the normal
# build/ never inherits the cached flag.
if(DEFINED CODEC_DIAG_ADOPT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CODEC_DIAG_ADOPT=${CODEC_DIAG_ADOPT})
endif()
