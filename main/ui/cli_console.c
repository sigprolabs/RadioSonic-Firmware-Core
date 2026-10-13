// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#include "cli_console.h"
#include "common.h"
#include "audio_pipeline.h"
#include "product_identity.h"

#ifndef APP_GIT_VERSION
#define APP_GIT_VERSION "dev"
#endif
#define BANNER_TITLE   PRODUCT_BANNER
#define BANNER_VERSION "Version " APP_GIT_VERSION
#include "audio_codec.h"
#include "input_guard.h"
#include "audio_dsp.h"
#include "dsp_effects.h"
#include "sd_storage.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "control_pot.h"
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_console.h"
#include "esp_log.h"
#include "argtable3/argtable3.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "linenoise/linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_vfs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"

// Terminal connection / line-editing state (see console_probe). The USB
// re-enumerates on reset and the user attaches the terminal while the board runs,
// so we (re)probe on connect rather than once at boot. Declared up here so the
// `term` diagnostic command can read them.
static volatile bool s_cdc0_dtr = false;   // CDC DTR (terminal open)
static volatile bool s_reprobe  = true;    // probe before first prompt / on reconnect
static int s_last_probe = -1;              // last linenoiseProbe() result

static bool codec_routing_readback(const char **name, bool *bias_on, bool *matches);

// --- Helper: Status Command ---
static int cmd_status(int argc, char **argv) {
    const dsp_effect_t *fx = dsp_current();

    printf(BANNER_TITLE "\n" BANNER_VERSION "\n\n");
    printf("--- System Status ---\n");
    printf("Input: %s\n",
        (g_state.input_mode == INPUT_MIC) ? "MIC" : "LINE IN (differential)");
    printf("Gain:   in %d dB, out %d dB\n",
           g_state.input_gain_db, g_state.output_gain_db);
    control_pot_print_status();   // prints nothing on boards without a pot
    printf("Codecs: main %s, exp1 %s, exp2 %s  [%s]\n",
           codec_main_probe_ok() ? "OK" : "probe FAILED",
           codec_present(CODEC_EXP1) ? "OK" : "--",
           codec_present(CODEC_EXP2) ? "OK" : "--",
           g_geom.tdm ? (g_geom.slot_bits == 16 ? "TDM 8x16 @ BCLK 6.144MHz"
                                                : "TDM 4x32 @ BCLK 6.144MHz")
                      : "I2S stereo");
    {
        // What the codec ACTUALLY holds (the Input line above is the firmware's
        // intent) — the first thing to check when LINE IN goes dead.
        const char *name; bool bias, match;
        if (codec_routing_readback(&name, &bias, &match)) {
            printf("Codec:  routing %s, MICBIAS %s (readback%s)\n", name, bias ? "on" : "off",
                   match ? " OK" : " MISMATCH — run `codec check`");
        } else {
            printf("Codec:  routing readback FAILED (SPI) — run `codec check`\n");
        }
        uint32_t spi_err = codec_spi_error_count();
        if (spi_err) printf("        %lu codec SPI errors since boot\n", (unsigned long)spi_err);
    }
    input_guard_print_status();
    printf("DSP:    %s\n", fx->label);
    if (fx->print) fx->print();
    {
        uint32_t last, avg, max;
        dsp_get_proc_stats(&last, &avg, &max);
        printf("Load:   %lu us/block avg, %lu max  (budget %d us)\n",
               (unsigned long)avg, (unsigned long)max,
               (int)((int64_t)g_geom.pp_frames * 1000000 / FS_HZ));
    }
    if (g_state.sd_card) {
        printf("SD:     Mounted\n");
    } else {
        printf("SD:     Not Mounted (last mount: %s — try `sd_remount`)\n",
               esp_err_to_name(sd_last_mount_err()));
    }

    // Internal-RAM budget check (see docs/ARCHITECTURE.md): largest block must
    // stay above the biggest late allocation (the httpd task stack); min-ever
    // is the lifetime low-water mark since boot.
    printf("Heap:   internal %u KB free (largest block %u KB, min-ever %u KB)\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    printf("PSRAM:  %u KB free (largest block %u KB)\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    // Deepest SD-download stack use — the evidence for how far the httpd stack
    // can be trimmed. Reads "not measured" until a download has actually run.
    size_t hdr = web_server_httpd_stack_headroom();
    if (hdr) {
        printf("httpd:  stack headroom %u B unused of %u after SD download\n",
               (unsigned)hdr, (unsigned)web_server_httpd_stack_size());
    } else {
        printf("httpd:  stack headroom not measured (run an SD download)\n");
    }

    bool wifi_disabled, wifi_is_ap;
    char wifi_ip[24];
    wifi_manager_get_wifi_status(&wifi_disabled, &wifi_is_ap, wifi_ip, sizeof(wifi_ip));
    if (wifi_disabled) {
        printf("WiFi:   Disabled\n");
    } else {
        printf("WiFi:   %s  IP: %s\n",
               wifi_is_ap ? "AP (" PRODUCT_AP_SSID ")" : "STA",
               wifi_ip);
    }
    return 0;
}

// --- Helper: Mode Command ---
static struct { struct arg_str *mode; struct arg_end *end; } mode_args;

// `mode` with no argument: the current mode, then every registered effect
// with its front-panel group and Mode LED color. A color that another
// group also uses is flagged, because the strip cannot tell the two apart.
static void mode_list(void)
{
    const dsp_effect_t *cur = dsp_current();
    printf("Current mode: %s\n", cur->name);
    printf("Available modes:");
    for (int i = 0; i < dsp_effect_count(); i++) printf(" %s", dsp_effect_at(i)->name);
    printf("\n\n  %-15s %-15s %-8s %s\n", "name", "group", "LED", "label");
    for (int i = 0; i < dsp_effect_count(); i++) {
        const dsp_effect_t *fx = dsp_effect_at(i);
        const bool bypass = dsp_is_bypass(fx);
        char led[12];
        if (bypass) snprintf(led, sizeof(led), "off");
        else        snprintf(led, sizeof(led), "#%06lX", (unsigned long)dsp_effect_led(fx));
        printf("%c %-15s %-15s %-8s %s%s\n", fx == cur ? '*' : ' ', fx->name,
               bypass ? "-" : dsp_effect_group(fx), led, fx->label ? fx->label : "",
               (!bypass && !fx->led_rgb) ? "  (default color: set led_rgb)" : "");
        int c = dsp_led_clash(fx);
        if (c >= 0) {
            printf("    ! same LED color as group '%s' (%s)\n",
                   dsp_effect_group(dsp_effect_at(c)), dsp_effect_at(c)->name);
        }
    }
}

static int cmd_mode(int argc, char **argv) {
    // Mirror cmd_input's argtable error pattern: report and fail, never
    // swallow. With no argument, list the effects instead.
    if (arg_parse(argc, argv, (void **)&mode_args) != 0) {
        arg_print_errors(stdout, mode_args.end, argv[0]);
        printf("Available modes:");
        for (int i = 0; i < dsp_effect_count(); i++) printf(" %s", dsp_effect_at(i)->name);
        printf("\n");
        return 1;
    }
    if (mode_args.mode->count == 0) {
        mode_list();
        return 0;
    }
    const char *m = mode_args.mode->sval[0];
    int idx = dsp_find(m);
    if (idx < 0) {
        printf("Unknown mode. Use:");
        for (int i = 0; i < dsp_effect_count(); i++) printf(" %s", dsp_effect_at(i)->name);
        printf("\n");
        return 1;
    }
    dsp_set(idx);
    printf("DSP Mode set to: %s\n", dsp_effect_at(idx)->name);
    return 0;
}

// --- Helper: Input Command ---
static struct { struct arg_str *mode; struct arg_end *end; } input_args;
static int cmd_input(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&input_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, input_args.end, argv[0]);
        printf("Usage: input mic|diff\n");
        return 1;
    }

    const char *m = input_args.mode->sval[0];

    if (strcmp(m, "mic") == 0) {
        g_state.input_mode  = INPUT_MIC;
        g_state.input_gain_db = 20;
    } else if (strcmp(m, "diff") == 0) {
        g_state.input_mode  = INPUT_LINE_DIFF;
        g_state.input_gain_db = 0;
    } else if (strcmp(m, "line") == 0) {
        printf("Single-ended line input was removed (the line jack is differential-only).\n"
               "Use: input diff\n");
        return 1;
    } else {
        printf("Unknown mode '%s'. Use: mic | diff\n", m);
        return 1;
    }

    codec_apply_settings();

    printf("Input set to %s, gain=%ddB\n",
           (g_state.input_mode == INPUT_MIC) ? "mic" : "diff", g_state.input_gain_db);
    return 0;
}



// --- Helper: Gain Command ---
// gain <db> | gain in <db> | gain out <db>
// Manual argv parsing — argtable3 would reject a negative
// output gain like "-6" as an unknown option flag.
static int cmd_gain(int argc, char **argv) {
    const char *usage = "Usage: gain <0-47> | gain in <0-47> | gain out <-6..29>\n";
    const char *which = "in";
    const char *valstr = NULL;

    if (argc == 2) {
        valstr = argv[1];                       // gain <db>  (input, legacy form)
    } else if (argc == 3 && (strcmp(argv[1], "in") == 0 || strcmp(argv[1], "out") == 0)) {
        which  = argv[1];
        valstr = argv[2];
    } else {
        printf("%s", usage);
        return 1;
    }

    char *endp;
    long val = strtol(valstr, &endp, 10);
    if (*endp) { printf("%s", usage); return 1; }

    if (strcmp(which, "out") == 0) {
        // AIC3204 HP/LO driver gain range: -6..+29 dB in 1 dB steps.
        if (val < -6 || val > 29) {
            printf("Error: output gain must be -6 to +29 dB\n");
            return 1;
        }
        g_state.output_gain_db = (int)val;
        codec_apply_output_gain();
        printf("Output gain set to %ld dB\n", val);
    } else {
        // AIC3204 MICPGA range is 0..+47.5 dB (0.5 dB steps); whole dB here.
        if (val < 0 || val > 47) {
            printf("Error: input gain must be 0-47 dB\n");
            return 1;
        }
        g_state.input_gain_db = (int)val;
        codec_apply_input_gain();
        printf("Input gain set to %ld dB\n", val);
    }
    return 0;
}

// --- Effect command dispatcher ---
// Effects own their console verbs (dsp_effect_t.cmd — dsp_effects.h); the
// console registers each hook-bearing effect's name as a command pointing
// here. esp_console gives us argv[0] = the command name = the effect name,
// so one dispatcher serves every effect, in-tree or out-of-tree.
static int cmd_effect_dispatch(int argc, char **argv)
{
    int idx = dsp_find(argv[0]);
    const dsp_effect_t *fx = (idx >= 0) ? dsp_effect_at(idx) : NULL;
    if (!fx || !fx->cmd) {
        printf("Error: no effect command '%s'\n", argv[0]);
        return 1;
    }
    return fx->cmd(argc, argv);
}

// --- Helper: SD Test ---
static int cmd_sdtest(int argc, char **argv) {
    if (!g_state.sd_card) { printf("No SD card mounted.\n"); return 1; }

    sdmmc_card_t *card = g_state.sd_card;

    // Card identity & speed
    printf("--- SD Card ---\n");
    printf("Name:     %s\n", card->cid.name);
    uint64_t cap_mb = ((uint64_t)card->csd.capacity * 512) / (1024 * 1024);
    printf("Type:     %s\n", cap_mb > 2048 ? "SDHC/SDXC" : "SDSC");
    printf("Capacity: %llu MB\n", cap_mb);
    printf("Speed:    %d kHz (max %lu kHz)\n", card->real_freq_khz, (unsigned long)card->max_freq_khz);

    // Filesystem free space
    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK) {
        printf("\n--- Filesystem (%s) ---\n", MOUNT_POINT);
        printf("Total:    %llu MB\n", total_bytes / (1024 * 1024));
        printf("Free:     %llu MB\n", free_bytes  / (1024 * 1024));
        printf("Used:     %llu MB\n", (total_bytes - free_bytes) / (1024 * 1024));
    } else {
        printf("esp_vfs_fat_info failed\n");
    }

    // Write/read round-trip sanity check (4 KB). Heap-allocated per run: two
    // static 4 KB buffers would be 8 KB of permanent internal .bss for an
    // occasional diagnostic — exactly what the internal-RAM ledger in
    // docs/ARCHITECTURE.md polices.
    printf("\n--- Write/Read Verify (4 KB) ---\n");
    const char *path = MOUNT_POINT "/sdtest_tmp.bin";
    uint8_t *wbuf = malloc(4096);
    uint8_t *rbuf = malloc(4096);
    if (!wbuf || !rbuf) { free(wbuf); free(rbuf); printf("OOM\n"); return 1; }
    for (int i = 0; i < 4096; i++) wbuf[i] = (uint8_t)i;

    const size_t vlen = 4096;
    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(path, "wb");
    if (!f) { printf("fopen write failed\n"); free(wbuf); free(rbuf); return 1; }
    fwrite(wbuf, 1, vlen, f);
    fclose(f);
    int64_t write_us = esp_timer_get_time() - t0;

    t0 = esp_timer_get_time();
    f = fopen(path, "rb");
    if (!f) { printf("fopen read failed\n"); free(wbuf); free(rbuf); return 1; }
    size_t n = fread(rbuf, 1, vlen, f);
    fclose(f);
    int64_t read_us = esp_timer_get_time() - t0;
    remove(path);

    bool match = (n == vlen) && (memcmp(wbuf, rbuf, vlen) == 0);
    printf("Write:    %lld us  (%.0f KB/s)\n", (long long)write_us,
           (float)vlen / 1024.0f / ((float)write_us / 1e6f));
    printf("Read:     %lld us  (%.0f KB/s)\n", (long long)read_us,
           (float)n / 1024.0f / ((float)read_us / 1e6f));
    printf("Verify:   %s\n", match ? "PASS" : "FAIL (data mismatch)");
    free(wbuf);
    free(rbuf);
    return match ? 0 : 1;
}

// --- Helper: Dump Buffer ---
static struct { struct arg_str *type; struct arg_end *end; } dump_args;
static int cmd_dump(int argc, char **argv) {
    arg_parse(argc, argv, (void **)&dump_args); // Optional arg parsing

    // Validate the target token — a typo like `dump rxx` must not silently
    // dump the wrong buffer (this CSV feeds host analysis scripts).
    bool dump_rx = false;
    if (dump_args.type->count > 0) {
        const char *t = dump_args.type->sval[0];
        if      (strcmp(t, "rx") == 0) dump_rx = true;
        else if (strcmp(t, "tx") == 0) dump_rx = false;
        else { printf("Usage: dump [rx|tx]  (default tx)\n"); return 1; }
    }

    const int32_t *buf = audio_pipeline_debug_buf(dump_rx, NULL);

    // Geometry-aware: rx buffers hold rx_ch interleaved channels (pair 0 =
    // main codec), tx always the stereo mixdown; 16-bit slot mode stores
    // int16 samples. Stereo output stays byte-identical to the legacy format
    // (host scripts parse the "Index, Left, Right" header).
    int ch = dump_rx ? g_geom.rx_ch : 2;
    if (ch == 2) {
        printf("Index, Left, Right\n");
    } else {
        printf("Index");
        for (int c = 0; c < ch; c++) printf(", ch%d", c);
        printf("\n");
    }
    for(int i=0; i<g_geom.pp_frames; i+=4) { // Decimate (skip samples) for faster print
        printf("%d", i);
        for (int c = 0; c < ch; c++) {
            long v = (g_geom.slot_bits == 16)
                       ? (long)((const int16_t *)buf)[i*ch + c]
                       : (long)buf[i*ch + c];
            printf(", %ld", v);
        }
        printf("\n");
        if(i % 64 == 0) vTaskDelay(1); // Keep WDT happy
    }
    return 0;
}

// --- Codec debug (TDM bring-up measurement verbs, lr_test protocol) ---
// Codec display names, indexed by codec_id_t (shared by the verb branches).
static const char *s_codec_names[CODEC_COUNT] = { "main", "exp1", "exp2" };

// Live input routing of the main codec, decoded from P1 R51/52/54/55/57 (the
// values codec_apply_settings writes). Returns false on SPI failure.
static bool codec_routing_readback(const char **name, bool *bias_on, bool *matches)
{
    uint8_t r51, r52, r54, r55, r57;
    if (codec_read_reg(CODEC_MAIN, 1, 51, &r51) != ESP_OK ||
        codec_read_reg(CODEC_MAIN, 1, 52, &r52) != ESP_OK ||
        codec_read_reg(CODEC_MAIN, 1, 54, &r54) != ESP_OK ||
        codec_read_reg(CODEC_MAIN, 1, 55, &r55) != ESP_OK ||
        codec_read_reg(CODEC_MAIN, 1, 57, &r57) != ESP_OK) return false;
    bool diff = (r52 == 0x10 && r54 == 0x10 && r55 == 0x04 && r57 == 0x04);
    bool mic  = (r52 == 0x40 && r54 == 0x01 && r55 == 0x40 && r57 == 0x40);
    *name = diff ? "diff (line in)" : mic ? "mic"
          : (!r52 && !r54 && !r55 && !r57) ? "UNROUTED (reset defaults)" : "OTHER";
    *bias_on = (r51 & 0x40) != 0;
    *matches = (g_state.input_mode == INPUT_MIC) ? (mic && *bias_on) : (diff && !*bias_on);
    return true;
}

static void check_report(uint8_t page, uint8_t reg, uint8_t wrote, uint8_t read, void *ctx)
{
    (void)ctx;
    printf("  MISMATCH P%u R%-3u wrote 0x%02X, codec holds 0x%02X\n",
           (unsigned)page, (unsigned)reg, wrote, read);
    vTaskDelay(pdMS_TO_TICKS(2));   // paced: console rule, no bursts
}

static int codec_id_arg(const char *s)
{
    for (int i = 0; i < CODEC_COUNT; i++) if (strcmp(s, s_codec_names[i]) == 0) return i;
    return -1;
}

static int cmd_codec(int argc, char **argv) {
    // `codec rd <page> <reg> [codec]` — one register, live.
    if (argc >= 4 && strcmp(argv[1], "rd") == 0) {
        int id = (argc >= 5) ? codec_id_arg(argv[4]) : CODEC_MAIN;
        long page = strtol(argv[2], NULL, 0), reg = strtol(argv[3], NULL, 0);
        uint8_t v;
        if (id < 0 || page < 0 || page > 255 || reg < 0 || reg > 127 ||
            codec_read_reg((codec_id_t)id, (uint8_t)page, (uint8_t)reg, &v) != ESP_OK) {
            printf("Usage: codec rd <page> <reg 0-127> [main|exp1|exp2]\n");
            return 1;
        }
        printf("%s P%ld R%ld = 0x%02X\n", s_codec_names[id], page, reg, v);
        return 0;
    }
    // `codec dump [0|1] [codec]` — page 0 and/or 1, 16 registers per line,
    // paced (console rule: no bulk printf bursts).
    if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        int first = 0, last = 1, id = CODEC_MAIN;
        for (int a = 2; a < argc; a++) {
            if (strcmp(argv[a], "0") == 0 || strcmp(argv[a], "1") == 0) first = last = argv[a][0] - '0';
            else if ((id = codec_id_arg(argv[a])) < 0) { printf("Usage: codec dump [0|1] [main|exp1|exp2]\n"); return 1; }
        }
        for (int page = first; page <= last; page++) {
            printf("%s page %d (reading P0 R42/R44/R45 clears those sticky flags):\n", s_codec_names[id], page);
            for (int row = 0; row < 128; row += 16) {
                char line[80];
                int n = snprintf(line, sizeof(line), "  %3d:", row);
                for (int r = row; r < row + 16; r++) {
                    uint8_t v;
                    if (codec_read_reg((codec_id_t)id, (uint8_t)page, (uint8_t)r, &v) != ESP_OK) {
                        printf("  SPI read failed at P%d R%d\n", page, r);
                        return 1;
                    }
                    n += snprintf(line + n, sizeof(line) - n, " %02X", v);
                }
                printf("%s\n", line);
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        return 0;
    }
    // `codec check [codec]` — is the codec in the state the firmware thinks?
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        int id = (argc >= 3) ? codec_id_arg(argv[2]) : CODEC_MAIN;
        if (id < 0) { printf("Usage: codec check [main|exp1|exp2]\n"); return 1; }
        uint8_t v1, v2;
        bool echo = codec_probe_raw((codec_id_t)id, &v1, &v2);
        printf("%s SPI echo: %02X/%02X %s\n", s_codec_names[id], v1, v2,
               echo ? "OK" : "FAILED (FF/FF = MISO not driven, 00/00 = MISO held low)");
        if (!echo) return 1;
        if (id == CODEC_MAIN) {
            const char *name; bool bias, match;
            if (codec_routing_readback(&name, &bias, &match))
                printf("Routing (readback): %s, MICBIAS %s — %s\n", name, bias ? "on" : "off",
                       match ? "matches firmware state" : "DOES NOT MATCH firmware state");
        }
        int bad = 0;
        int n = codec_check_shadow((codec_id_t)id, check_report, NULL, &bad);
        if (n < 0) { printf("Shadow compare: SPI read failed\n"); return 1; }
        printf("Shadow compare: %d registers the firmware wrote, %d differ\n", n, bad);
        static const struct { uint8_t page, reg; const char *what; } flags[] = {
            { 0, 36, "ADC/PGA flags" }, { 0, 37, "DAC/driver flags" },
            { 0, 42, "sticky overflow flags (cleared by this read)" },
            { 0, 44, "sticky driver/headset flags (cleared by this read)" },
            { 1, 2,  "LDO control/status" }, { 1, 62, "PGA gain-applied flags" },
        };
        printf("Status registers (compare with a healthy board):\n");
        for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
            uint8_t v;
            if (codec_read_reg((codec_id_t)id, flags[i].page, flags[i].reg, &v) == ESP_OK)
                printf("  P%u R%-3u = 0x%02X  %s\n", (unsigned)flags[i].page,
                       (unsigned)flags[i].reg, v, flags[i].what);
        }
        printf("SPI errors since boot: %lu\n", (unsigned long)codec_spi_error_count());
        return 0;
    }
    // `codec snapshot` — the registers as they were BEFORE this boot's init
    // (the codec stays powered across an MCU reset), with every register that
    // differs from the live value now marked '*'.
    if (argc >= 2 && strcmp(argv[1], "snapshot") == 0) {
        const uint8_t *s0, *s1;
        if (!codec_boot_snapshot(0, &s0) || !codec_boot_snapshot(1, &s1)) {
            printf("No boot snapshot (SPI read failed at boot).\n");
            return 1;
        }
        printf("Boot snapshot (read before init): codec %s; %s\n",
               codec_boot_was_running() ? "WAS RUNNING before this boot (MCU-only reset)"
                                        : "was fresh (power-on)",
               codec_was_adopted() ? "state ADOPTED (diag build, no init)" : "then re-initialised");
        bool diff = s1[52] == 0x10 && s1[54] == 0x10 && s1[55] == 0x04 && s1[57] == 0x04;
        bool mic  = s1[52] == 0x40 && s1[54] == 0x01 && s1[55] == 0x40 && s1[57] == 0x40;
        printf("  routing %s, MICBIAS %s | P0 R36=%02X R37=%02X R42=%02X R44=%02X R63=%02X R81=%02X R82=%02X\n",
               diff ? "diff" : mic ? "mic" : "OTHER", (s1[51] & 0x40) ? "on" : "off",
               s0[36], s0[37], s0[42], s0[44], s0[63], s0[81], s0[82]);
        printf("  P1 R1=%02X R2=%02X R10=%02X R58=%02X R59=%02X R60=%02X R61=%02X R62=%02X R71=%02X R123=%02X\n",
               s1[1], s1[2], s1[10], s1[58], s1[59], s1[60], s1[61], s1[62], s1[71], s1[123]);
        for (int page = 0; page < 2; page++) {
            const uint8_t *s = page ? s1 : s0;
            printf("page %d (snapshot; '*' = differs from live now):\n", page);
            for (int row = 0; row < 128; row += 16) {
                char line[96];
                int n = snprintf(line, sizeof(line), "  %3d:", row);
                for (int r = row; r < row + 16; r++) {
                    uint8_t v = s[r];
                    bool differs = (codec_read_reg(CODEC_MAIN, (uint8_t)page, (uint8_t)r, &v) == ESP_OK) && v != s[r];
                    n += snprintf(line + n, sizeof(line) - n, "%c%02X", differs ? '*' : ' ', s[r]);
                }
                printf("%s\n", line);
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        return 0;
    }
#if CODEC_DIAG_ADOPT
    // `codec wr <page> <reg> <val> [codec]` — diagnostic builds only.
    if (argc >= 5 && strcmp(argv[1], "wr") == 0) {
        int id = (argc >= 6) ? codec_id_arg(argv[5]) : CODEC_MAIN;
        long page = strtol(argv[2], NULL, 0), reg = strtol(argv[3], NULL, 0), val = strtol(argv[4], NULL, 0);
        if (id < 0 || page < 0 || page > 255 || reg < 1 || reg > 127 || val < 0 || val > 255) {
            printf("Usage: codec wr <page> <reg 1-127> <val> [main|exp1|exp2]\n");
            return 1;
        }
        codec_write_reg((codec_id_t)id, (uint8_t)page, (uint8_t)reg, (uint8_t)val);
        uint8_t v = 0xEE;
        codec_read_reg((codec_id_t)id, (uint8_t)page, (uint8_t)reg, &v);
        printf("%s P%ld R%ld <- 0x%02lX (reads back 0x%02X)\n", s_codec_names[id], page, reg, val, v);
        return 0;
    }
#endif
    // Recovery rungs — try in this order when LINE IN is dead; the first one
    // that brings it back tells you which layer failed.
    if (argc >= 2 && strcmp(argv[1], "reroute") == 0) {
        codec_reroute_inputs();
        printf("Routing + gains re-applied for %s (no precharge).\n",
               g_state.input_mode == INPUT_MIC ? "mic" : "diff");
        return 0;
    }
    // `codec recover [ms]` — the dead-LINE-IN fix on demand: precharge every
    // input to common mode (default: `codec precharge`), then re-route.
    if (argc >= 2 && strcmp(argv[1], "recover") == 0) {
        long ms = (argc >= 3) ? strtol(argv[2], NULL, 10) : 0;
        if (ms < 0 || ms > 10000) { printf("Usage: codec recover [precharge ms, 0-10000]\n"); return 1; }
        if (ms == 0) ms = (long)codec_get_precharge_ms();
        codec_recover_inputs((uint32_t)ms);
        printf("Inputs precharged to common mode (%ld ms) and re-routed for %s.\n",
               ms, g_state.input_mode == INPUT_MIC ? "mic" : "diff");
        return 0;
    }
    // `codec precharge [ms]` — how long a pin is held at common mode before it
    // may be routed (boot, wake, recover). Bench tuning only; not saved.
    if (argc >= 2 && strcmp(argv[1], "precharge") == 0) {
        if (argc >= 3) {
            long ms = strtol(argv[2], NULL, 10);
            if (ms < 0 || ms > 10000) { printf("Usage: codec precharge [ms, 0-10000]\n"); return 1; }
            codec_set_precharge_ms((uint32_t)ms);
        }
        printf("Input precharge: %lu ms (default 3000; not saved)\n",
               (unsigned long)codec_get_precharge_ms());
        return 0;
    }
    // `codec guard [on|off|test]` — the stuck-input guard (input_guard.c).
    // `test` makes the guard task run its recovery branch once (a real
    // `codec recover` with the default precharge) so the stack line below
    // reflects the deepest path, which otherwise only runs on a real fault.
    if (argc >= 2 && strcmp(argv[1], "guard") == 0) {
        if (argc >= 3 && strcmp(argv[2], "on") == 0)        input_guard_enable(true);
        else if (argc >= 3 && strcmp(argv[2], "off") == 0)  input_guard_enable(false);
        else if (argc >= 3 && strcmp(argv[2], "test") == 0) {
            input_guard_test();
            printf("Guard test queued: the guard task runs `codec recover` within ~1 s "
                   "(input drops for the precharge); run `codec guard` again after ~7 s.\n");
        }
        else if (argc >= 3) { printf("Usage: codec guard [on|off|test]\n"); return 1; }
        input_guard_print_status();
        printf("        guard task stack: %u B never used of %u\n",
               input_guard_stack_free(), input_guard_stack_size());
        return 0;
    }
    // `codec cm [0.75|0.9]` — full-chip common mode (SLAA557 3.5: 0.75 V below
    // 1.8 V AVDD). Setting it re-initialises the codec (click + full precharge).
    if (argc >= 2 && strcmp(argv[1], "cm") == 0) {
        if (argc >= 3) {
            if (strcmp(argv[2], "0.75") == 0)     codec_set_cm_075(true);
            else if (strcmp(argv[2], "0.9") == 0) codec_set_cm_075(false);
            else { printf("Usage: codec cm [0.75|0.9]\n"); return 1; }
            codec_reinit();
        }
        printf("Common mode: %s V%s\n", codec_get_cm_075() ? "0.75" : "0.9",
               codec_get_cm_075() ? "" : "  (outside SLAA557 3.5 at this board's 1.72 V AVDD; A/B tests only, not saved)");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "adc") == 0) {
        codec_adc_cycle();
        printf("ADC power-cycled (off 50 ms), routing untouched.\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "reinit") == 0) {
        codec_reinit();
        printf("Codec soft reset + full re-init done (expect a click).\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        // Clock sweep of the page-select round trip. Reads fail at a much
        // lower clock than writes (AIC3204 tv(DOUT) 18 ns + the GPIO-matrix
        // round trip vs. a half-period sampling window), so the read ceiling
        // gets measured here rather than assumed — the boot presence probe
        // must run at least one rung below the highest OK row.
        static const int sweep_hz[] = { 1000000, 2000000, 4000000, 5000000,
                                        8000000, 10000000, 13000000,
                                        16000000, 20000000 };
        // `codec probe <hz>` narrows the sweep to one clock (retry / soak).
        int single = (argc >= 3) ? (int)strtol(argv[2], NULL, 10) : 0;
        const int *list = single ? &single : sweep_hz;
        int n = single ? 1 : (int)(sizeof(sweep_hz) / sizeof(sweep_hz[0]));

        // Production handles FIRST, before any temporary device exists on the
        // bus: a temp device re-routes the same CS pin through a different hw
        // CS slot, and if IDF does not restore the routing on removal it would
        // itself explain a later failure. Reading before and after separates
        // "production handle is broken" from "the sweep broke it".
        for (int id = 0; id < CODEC_COUNT; id++) {
            uint8_t v1, v2;
            bool ok = codec_probe_raw((codec_id_t)id, &v1, &v2);
            printf("  boot handle %s (pre-sweep): %02X/%02X %s\n",
                   s_codec_names[id], v1, v2, ok ? "OK" : "--");
        }

        printf("SPI read sweep (write page 01 -> read, write 00 -> read):\n");
        printf("  request  actual");
        for (int id = 0; id < CODEC_COUNT; id++) printf("  %-10s", s_codec_names[id]);
        printf("\n");
        for (int i = 0; i < n; i++) {
            printf("  %5d kHz", list[i] / 1000);
            for (int id = 0; id < CODEC_COUNT; id++) {
                uint8_t v1, v2; int khz = 0;
                bool ok = codec_probe_raw_at((codec_id_t)id, list[i], &khz, &v1, &v2);
                if (id == 0) printf(" %6d", khz);
                printf("  %02X/%02X %-4s", v1, v2, ok ? "OK" : "--");
            }
            printf("\n");
        }
        // The sweep's temporary devices stole each CS pin's routing; give the
        // boot handles working CS back before anything else touches a codec.
        codec_spi_reattach();
        for (int id = 0; id < CODEC_COUNT; id++) {
            uint8_t v1, v2;
            bool ok = codec_probe_raw((codec_id_t)id, &v1, &v2);
            printf("  boot handle %s (post-sweep, reattached): %02X/%02X %s\n",
                   s_codec_names[id], v1, v2, ok ? "OK" : "--");
        }
        printf("Boot handle clocks: main 20000 kHz, exp 10000 kHz.\n");
        printf("Diagnostic only — boot presence/geometry unchanged; TDM needs a reboot.\n"
               "OK = codec echoed both values. FF/FF = MISO not driven: an absent\n"
               "board, or CS not asserting. 00/00 = MISO stuck low. EE/EE = driver\n"
               "error. Measured 2026-07-27 (main codec, J5 off): reads are clean at\n"
               "every clock 1-20 MHz, so there is no read-timing ceiling below the\n"
               "AIC3204's own tsck 50 ns / 20 MHz write limit on the main board.\n");
        return 0;
    }
    // `codec cs <main|exp1|exp2> <0|1>` parks a CS line at a static level so it
    // can be metered at the ESP pin and at the far side of J5; `codec cs release`
    // hands the pins back to the SPI peripheral. SPI is dead for a parked codec.
    if (argc >= 3 && strcmp(argv[1], "cs") == 0) {
        if (strcmp(argv[2], "release") == 0) {
            codec_spi_reattach();
            printf("CS pins returned to the SPI peripheral.\n");
            return 0;
        }
        if (argc < 4) {
            printf("Usage: codec cs <main|exp1|exp2> <0|1> | codec cs release\n");
            return 1;
        }
        int id = -1;
        for (int i = 0; i < CODEC_COUNT; i++)
            if (strcmp(argv[2], s_codec_names[i]) == 0) id = i;
        int level = (int)strtol(argv[3], NULL, 10);
        int pin;
        if (id < 0 || !codec_cs_pin((codec_id_t)id, &pin)) {
            printf("Usage: codec cs <main|exp1|exp2> <0|1> | codec cs release\n");
            return 1;
        }
        codec_cs_park((codec_id_t)id, level);
        printf("%s CS = GPIO%d parked %s (%s).\n", s_codec_names[id], pin,
               level ? "HIGH" : "LOW", level ? "~3.3 V" : "~0 V");
        printf("Meter GPIO%d at the ESP and at the J5 expansion end — they must\n"
               "agree. SPI to %s is dead until `codec cs release`.\n",
               pin, s_codec_names[id]);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "r63") == 0) {
        bool straight = (strcmp(argv[2], "straight") == 0);
        if (!straight && strcmp(argv[2], "cross") != 0) {
            printf("Usage: codec r63 <straight|cross>\n");
            return 1;
        }
        codec_dbg_r63(straight);
        printf("R63 DAC mapping: %s\n", straight ? "straight" : "cross");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "offset") == 0) {
        char *endp;
        long base = strtol(argv[2], &endp, 10);
        if (*endp || base < 0 || base > 255) {
            printf("Usage: codec offset <base 0-255>  (BCLKs; default 0 — measured 2026-07-27)\n");
            return 1;
        }
        codec_dbg_tdm_base((uint8_t)base);
        printf("TDM data offsets rewritten from base %ld BCLKs\n", base);
        return 0;
    }
    printf("Codecs: main %s, exp1 %s, exp2 %s\n",
           codec_main_probe_ok() ? "OK" : "probe FAILED",
           codec_present(CODEC_EXP1) ? "present" : "--",
           codec_present(CODEC_EXP2) ? "present" : "--");
    printf("Mode:   %s, %d ch RX, %d-bit slots, %d frames/block (%u B RX / %u B TX)\n",
           g_geom.tdm ? "TDM" : "std I2S", g_geom.rx_ch, g_geom.slot_bits,
           g_geom.pp_frames, (unsigned)g_geom.rx_block_bytes,
           (unsigned)g_geom.tx_block_bytes);
    printf("Verbs:  codec check | codec snapshot | codec dump [0|1] | codec rd <page> <reg>\n"
           "        codec recover [ms]   (dead LINE IN: precharge inputs to CM, re-route)\n"
           "        codec guard [on|off|test] | codec precharge [ms] | codec cm [0.75|0.9]\n"
           "        codec reroute | codec adc | codec reinit   (other recovery rungs)\n"
           "        codec probe [hz] | codec cs <id> <0|1>|release\n"
           "        codec r63 <straight|cross> | codec offset <base>\n");
    return 0;
}

// Plain reboot (no NVS side effects, unlike `wifi ap`/web restart).
static int cmd_restart(int argc, char **argv) {
    printf("Restarting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

// --- TDM mixdown gains ---
static int cmd_mixgain(int argc, char **argv) {
    if (argc < 3) {
        printf("Mix gains: pair0=%.2f pair1=%.2f pair2=%.2f  (pair 0 = main codec)\n",
               dsp_get_mix_gain(0), dsp_get_mix_gain(1), dsp_get_mix_gain(2));
        printf("Usage: mixgain <pair 0-2> <gain>   (sum is hard-clipped; not persisted)\n");
        return (argc < 2) ? 0 : 1;
    }
    char *endp;
    long pair = strtol(argv[1], &endp, 10);
    if (*endp || pair < 0 || pair > 2) { printf("pair must be 0-2\n"); return 1; }
    float g = strtof(argv[2], &endp);
    if (*endp || g < -8.0f || g > 8.0f) { printf("gain must be -8..8\n"); return 1; }
    dsp_set_mix_gain((int)pair, g);
    printf("mixgain pair%ld = %.2f\n", pair, g);
    return 0;
}

// --- Version Command ---
static int cmd_ver(int argc, char **argv) {
    printf(BANNER_TITLE "\n" BANNER_VERSION "\n");
    // Config fingerprint: sdkconfig is untracked and only seeded from
    // sdkconfig.defaults on first configure, so a stale local file silently
    // drifts (observed: FREERTOS_HZ=100 on another dev's rig throttled `xfer
    // live` to ~100 KB/s). Print the perf-critical values so drift is visible
    // on the board itself — including the board revision, a menuconfig-only
    // choice that no defaults file carries.
    printf("Board: %s build\n", BOARD_NAME);
    printf("Config: tick %d Hz, CDC TX/RX buf %d/%d B\n",
           CONFIG_FREERTOS_HZ,
           CONFIG_TINYUSB_CDC_TX_BUFSIZE, CONFIG_TINYUSB_CDC_RX_BUFSIZE);
    return 0;
}

// --- Reboot into ROM serial/USB download mode ---
// With the console on TinyUSB (USB-OTG), the USB-Serial-JTAG auto-download path
// is gone, so flashing normally needs the BOOT button. This command forces the
// ROM bootloader into download mode on the next reset: set RTC_CNTL_FORCE_
// DOWNLOAD_BOOT (survives the reset in the RTC domain) then software-reset. The
// device re-enumerates as the ESP32-S3 "USB JTAG/serial debug unit" so esptool /
// idf.py flash can program it without touching BOOT.
static int cmd_bootloader(int argc, char **argv) {
    (void)argc; (void)argv;
    // Entering download mode is a two-hazard transition (both observed,
    // 2026-07-03/04, and originally misdiagnosed as firmware wedges):
    //  1. Resetting with the OTG session live leaves the host holding a stale
    //     CDC enumeration — the download device never appears and the board
    //     sits in ROM download mode looking dead (frozen LEDs, no WiFi).
    //     Fixed by tud_disconnect(): a proper bus detach first.
    //  2. The ROM's USB-Serial-JTAG maps host DTR/RTS onto the EN/BOOT
    //     auto-reset emulation — a host that still holds the port (lines
    //     asserted) through the PHY handoff strangles enumeration. So wait
    //     for the host to CLOSE the port (DTR drop) before resetting, with a
    //     timeout so an unattended board still proceeds.
    printf("Close the serial port now — entering download mode when it drops"
           " (or after 5 s)...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(150));    // let the message drain over USB
    for (int i = 0; i < 500 && s_cdc0_dtr; i++) vTaskDelay(pdMS_TO_TICKS(10));
    tud_disconnect();                  // clean bus detach (hazard 1)
    vTaskDelay(pdMS_TO_TICKS(600));    // let the host process the detach

    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_rom_software_reset_system();
    return 0;                          // not reached
}

// --- Terminal diagnostics ---
// `term`        — show fds, dumb/smart state, last probe result, DTR.
// `term query`  — write ESC[6n (cursor-position report) and show the raw reply
//                 within 500 ms. Smart-mode EDITING relies on this query at
//                 runtime; note the smart/dumb PROBE itself sends ESC[5n
//                 (status report) — a terminal can answer one and not the
//                 other, which is exactly what this diagnostic distinguishes.
// `term smart`  / `term dumb` — toggle line-editing mode live.
static int cmd_term(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "query") == 0) {
        printf("Sending ESC[6n, waiting up to 500 ms for a reply...\n");
        fflush(stdout);
        const char q[] = "\x1b[6n";
        write(fileno(stdout), q, 4);
        // (no fsync — the CDC VFS has no .fsync; the write above already
        // queues and the CDC path flushes on its own)
        uint8_t buf[16];
        int n = 0;
        int64_t deadline = esp_timer_get_time() + 500000;
        while (n < (int)sizeof(buf) && esp_timer_get_time() < deadline) {
            size_t rx = 0;
            tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf + n, sizeof(buf) - n, &rx);
            if (rx > 0) n += (int)rx;
            else vTaskDelay(pdMS_TO_TICKS(10));
        }
        printf("reply: %d bytes:", n);
        for (int i = 0; i < n; i++) printf(" %02x", buf[i]);
        printf("%s\n", n == 0 ? "  (none — terminal did not answer)" : "");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "smart") == 0) { linenoiseSetDumbMode(0); printf("line editing: smart\n"); return 0; }
    if (argc >= 2 && strcmp(argv[1], "dumb")  == 0) { linenoiseSetDumbMode(1); printf("line editing: dumb\n");  return 0; }

    printf("fds: stdin=%d stdout=%d stderr=%d\n", fileno(stdin), fileno(stdout), fileno(stderr));
    printf("mode=%s  last_probe=%d  dtr=%d\n",
           linenoiseIsDumbMode() ? "dumb" : "smart", s_last_probe, (int)s_cdc0_dtr);
    printf("usage: term | term query | term smart | term dumb\n");
    return 0;
}

// --- ADC test command (issue #7: R22 pot bring-up) ---
// `adc <gpio> [n]` — one-shot ADC read (averaged over n samples, default 16)
// with calibrated millivolts when the fitting scheme is available.
//
// HARDWARE CONSTRAINT: on the ESP32-S3 only GPIO1-10 (ADC1) and GPIO11-20
// (ADC2) are ADC-capable. IO42 (the R22 pot per issue #7) has NO ADC — for
// non-ADC pins this command falls back to reporting the digital level.
// Note ADC2 is unavailable while WiFi is active (shared SAR hardware);
// prefer ADC1 pins (GPIO1-10) for any analog rework.
static int cmd_adc(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: adc <gpio> [samples]\n"
               "ADC-capable pins (ESP32-S3): GPIO1-10 (ADC1), GPIO11-20 (ADC2;\n"
               "unavailable while WiFi is on). Other pins: digital level only.\n");
        return 1;
    }
    char *endp;
    int gpio = (int)strtol(argv[1], &endp, 10);
    if (*endp || gpio < 0 || gpio > 48) { printf("Error: bad gpio\n"); return 1; }
    int nsamp = 16;
    if (argc >= 3) {
        nsamp = (int)strtol(argv[2], &endp, 10);
        if (*endp || nsamp < 1 || nsamp > 1000) { printf("Error: samples 1-1000\n"); return 1; }
    }

    adc_unit_t unit;
    adc_channel_t chan;
    if (adc_oneshot_io_to_channel(gpio, &unit, &chan) != ESP_OK) {
        // Not an ADC pin (e.g. IO42/R22): report the digital level instead.
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << gpio,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        printf("GPIO%d is not ADC-capable on the ESP32-S3 (ADC1: GPIO1-10,\n"
               "ADC2: GPIO11-20). Digital level: %d\n"
               "For an analog pot reading, route the signal to an ADC1 pin.\n",
               gpio, gpio_get_level(gpio));
        return 1;
    }

    // IDF allows one oneshot handle per ADC unit, and control_pot.c holds ADC1
    // for the whole session on boards with a pot. Borrow that handle instead of
    // failing to create a second one; only delete a unit we opened ourselves.
    adc_oneshot_unit_handle_t h = (unit == ADC_UNIT_1) ? control_pot_adc_unit() : NULL;
    bool own_unit = false;
    if (!h) {
        adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
        esp_err_t err = adc_oneshot_new_unit(&ucfg, &h);
        if (err != ESP_OK) {
            printf("adc unit init failed: %s%s\n", esp_err_to_name(err),
                   unit == ADC_UNIT_2 ? " (ADC2 is unavailable while WiFi is on)" : "");
            return 1;
        }
        own_unit = true;
    }
    adc_oneshot_chan_cfg_t ccfg = {
        .atten    = ADC_ATTEN_DB_12,          // full 0..~3.1V input range
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    // Graceful like the unit-init above — a CLI diagnostic must never be able
    // to panic the board (ESP_ERROR_CHECK here would abort on a driver error).
    esp_err_t cerr = adc_oneshot_config_channel(h, chan, &ccfg);
    if (cerr != ESP_OK) {
        printf("ADC channel config failed: %s\n", esp_err_to_name(cerr));
        if (own_unit) adc_oneshot_del_unit(h);
        return 1;
    }

    // Optional calibration (curve fitting is the S3 scheme).
    adc_cali_handle_t cali = NULL;
    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id  = unit,
        .chan     = chan,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    bool have_cali = (adc_cali_create_scheme_curve_fitting(&cal_cfg, &cali) == ESP_OK);

    int64_t sum = 0;
    int raw = 0, ok_reads = 0;
    for (int i = 0; i < nsamp; i++) {
        if (adc_oneshot_read(h, chan, &raw) == ESP_OK) { sum += raw; ok_reads++; }
        vTaskDelay(1);
    }

    if (ok_reads == 0) {
        printf("adc read failed%s\n",
               unit == ADC_UNIT_2 ? " (ADC2 is unavailable while WiFi is on)" : "");
    } else {
        int avg = (int)(sum / ok_reads);
        printf("GPIO%d (ADC%d ch%d): raw=%d (avg of %d)", gpio, unit + 1, chan,
               avg, ok_reads);
        int mv = 0;
        if (have_cali && adc_cali_raw_to_voltage(cali, avg, &mv) == ESP_OK) {
            printf("  =  %d mV", mv);
        }
        printf("\n");
    }

    if (have_cali) adc_cali_delete_scheme_curve_fitting(cali);
    if (own_unit) adc_oneshot_del_unit(h);   // never delete the pot's handle
    return ok_reads ? 0 : 1;
}

// --- Command registration (shared by the console task) ---
static void register_console_commands(void) {
    esp_console_register_help_command();

    const esp_console_cmd_t c_status = { .command="status", .help="Show System Status", .func=&cmd_status };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_status));

    // The <mode> hint is built from the registry, so a newly registered effect
    // shows up here with no edits (a hand-written list once drifted and
    // missed an effect for weeks).
    static char mode_hint[160];
    {
        int off = 0;
        for (int i = 0; i < dsp_effect_count() && off < (int)sizeof(mode_hint) - 2; i++) {
            off += snprintf(mode_hint + off, sizeof(mode_hint) - off, "%s%s",
                            i ? "|" : "", dsp_effect_at(i)->name);
        }
    }
    mode_args.mode = arg_str0(NULL, NULL, "<mode>", mode_hint);
    mode_args.end = arg_end(1);
    const esp_console_cmd_t c_mode = { .command="mode",
        .help="Set the DSP mode; with no argument, list the effects with their group and LED color",
        .func=&cmd_mode, .argtable=&mode_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_mode));

    input_args.mode = arg_str1(NULL, NULL, "<mode>", "mic | diff (line in, differential)");
    input_args.end = arg_end(1);
    const esp_console_cmd_t c_input = { .command="input", .help="Set Input Source", .func=&cmd_input, .argtable=&input_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_input));

    // Manual argv parsing (see cmd_gain) — negative output gain would be
    // rejected by argtable3 as an option flag.
    const esp_console_cmd_t c_gain = {
        .command  = "gain",
        .help     = "Gain: gain <0-47> (input) | gain in <0-47> | gain out <-6..29>",
        .func     = &cmd_gain,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_gain));

    // Effect commands come from the registry: every registered effect with
    // a cmd hook gets its name
    // registered as a console command, routed through one dispatcher. This
    // runs in start_console_task(), after dsp_init() has called
    // rs_user_effects_init(), so the registry is complete. No argtable —
    // effect verbs routinely take negative numbers.
    for (int i = 0; i < dsp_effect_count(); i++) {
        const dsp_effect_t *fx = dsp_effect_at(i);
        if (!fx->cmd) continue;
        const esp_console_cmd_t c_fx = {
            .command  = fx->name,
            .help     = fx->label,
            .func     = &cmd_effect_dispatch,
            .argtable = NULL
        };
        if (esp_console_cmd_register(&c_fx) != ESP_OK) {
            printf("W: effect command '%s' collides with a core command — not registered\n",
                   fx->name);
        }
    }

    dump_args.type = arg_str0(NULL, NULL, "<type>", "rx | tx");
    dump_args.end = arg_end(1);
    const esp_console_cmd_t c_dump = { .command="dump", .help="Dump buffer CSV", .func=&cmd_dump, .argtable=&dump_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_dump));

    const esp_console_cmd_t c_sd = { .command="sdtest", .help="SD card info, free space, write/read verify", .func=&cmd_sdtest };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_sd));

    const esp_console_cmd_t c_codec = { .command="codec", .help="Codec info; check | dump [0|1] | rd <pg> <reg> (register readback) | recover [ms] (dead LINE IN fix) | guard [on|off|test] | precharge [ms] | cm [0.75|0.9] | reroute | adc | reinit (recovery) | probe [hz] | r63 <straight|cross> | offset <base> | cs <id> <0|1>|release (bring-up)", .func=&cmd_codec };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_codec));

    const esp_console_cmd_t c_restart = { .command="restart", .help="Reboot the board (no settings changed)", .func=&cmd_restart };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_restart));

    const esp_console_cmd_t c_mixgain = { .command="mixgain", .help="TDM mixdown gains: mixgain <pair 0-2> <gain>", .func=&cmd_mixgain };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_mixgain));

    const esp_console_cmd_t c_ver = { .command="ver", .help="Show firmware version", .func=&cmd_ver };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_ver));

    const esp_console_cmd_t c_bootloader = { .command="bootloader", .help="Reboot into ROM download mode for flashing (no BOOT button needed)", .func=&cmd_bootloader };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_bootloader));

    const esp_console_cmd_t c_term = { .command="term", .help="Terminal diagnostics: term | term query | term smart | term dumb", .func=&cmd_term };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_term));

    const esp_console_cmd_t c_adc = { .command="adc", .help="ADC test read: adc <gpio> [samples] (S3: ADC1=GPIO1-10)", .func=&cmd_adc };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c_adc));
}

// --- TinyUSB CDC console bring-up ---
//
// The single native USB port is owned by TinyUSB (USB-OTG), not USB-Serial-JTAG:
// the two controllers share one PHY and are mutually exclusive. The console runs
// over CDC channel 0. Phase 2 adds CDC1 for fast binary transfer.
//
// fd-rotation fix (the Feb-2026 failure): with CONFIG_ESP_CONSOLE_NONE nothing
// pre-binds stdio fds 0/1/2 at startup, so tinyusb_console_init() is the SINGLE
// thing that binds them — they land as stdin=0/stdout=1/stderr=2. We drive
// linenoise + esp_console_run() manually instead of esp_console_new_repl_*,
// because the new_repl_* helpers re-open their own device VFS and re-dup the fds.
static const char *CONSOLE_TAG = "console";

// Blocking stdio backend over TinyUSB CDC0.
//
// The esp_tinyusb CDC VFS is non-blocking ONLY (its read returns EWOULDBLOCK and
// it ignores O_NONBLOCK changes), which makes linenoise spin: the edit loop needs
// a blocking single-char read. We register our own VFS that honors O_NONBLOCK via
// fcntl — blocking during line editing, non-blocking during linenoise's terminal
// probe — so linenoise + esp_console work unchanged (history, completion, hints).
//
// The read seam (linenoise.c): the edit loop and getCursorPosition() call the
// settable `read_func`; only linenoiseProbe() uses a bare read() with O_NONBLOCK
// toggled directly. We install our own read_func (via the weak
// linenoiseSetReadCharacteristics override) so the edit-loop read blocks reliably:
// it MUST return exactly the bytes read and must NOT return 0 for "no data yet",
// because linenoiseEdit submits the line on any read <= 0. Even the DTR 0->1
// (re)connect event — which breaks a blocked edit so the task loop can
// re-probe + re-banner — returns -1 with errno=EIO, never 0: a 0-return there
// made linenoiseDumb treat the break as a submitted byte and self-feed, which
// corrupted the heap (see the MUST-return--1 comment at the return site).
// (This console was brought up incrementally over the officially-unsupported
// TinyUSB CDC transport — raw CDC → stdio redirect → esp_console/linenoise dumb →
// smart-mode probe. See docs/TINYUSB_CONSOLE.md for the journey and root causes.)
#define CDC_CONSOLE_VFS_PATH "/dev/cdc0"

static SemaphoreHandle_t s_cdc0_rx_sem;   // given on CDC RX; waited on by a blocking read
static SemaphoreHandle_t s_cdc0_tx_mtx;   // serializes CDC writes from multiple tasks (logs)
static int s_cdc0_open_fd = 0;            // hands out distinct local fds for stdin/out/err
static int s_cdc0_flags[3] = { O_RDWR, O_RDWR, O_RDWR };  // per-fd flags (O_NONBLOCK)
static inline int cdc0_fd_clamp(int fd) { return (fd < 0 || fd > 2) ? 2 : fd; }
static volatile bool s_editing = false;   // true while blocked in linenoise() (log-redraw guard)
static int s_console_max_line = 256;      // linenoise line-buffer size (from esp_console cfg)
static int s_dumb_line_len   = 0;         // dumb-mode line length, for the over-read guard

static void cdc0_line_state_changed(int itf, cdcacm_event_t *event)
{
    (void)itf;
    bool dtr = event->line_state_changed_data.dtr;
    if (dtr && !s_cdc0_dtr) {
        s_reprobe = true;                        // 0->1: terminal (re)connected
        if (s_cdc0_rx_sem) xSemaphoreGive(s_cdc0_rx_sem);  // wake a blocked reader now
    }
    s_cdc0_dtr = dtr;
}

// RX callback runs in the TinyUSB task context — wake any blocking reader.
static void cdc0_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)itf; (void)event;
    if (s_cdc0_rx_sem) xSemaphoreGive(s_cdc0_rx_sem);
}

// Install TinyUSB (full-speed OTG) + CDC0. This is where the shared USB PHY moves
// from USB-Serial-JTAG to USB-OTG. Single CDC: fast file transfer reuses CDC0 in a
// raw-binary mode (see serial_xfer.c) rather than a 2nd CDC — a 2-CDC composite
// destabilized CDC0 on this host, and the throughput comes from TinyUSB either way.
// USB string descriptors, in the order esp_tinyusb's default device and
// configuration descriptors index them: 0 language (US English),
// 1 manufacturer, 2 product, 3 serial number, 4 CDC interface. The names come
// from product_identity.h. The serial stays the esp_tinyusb default "123456":
// Windows keys the COM port number on it, so changing it would move every
// board to a new port.
static const char s_usb_langid[] = { 0x09, 0x04 };
static const char *s_usb_strings[] = {
    s_usb_langid,
    PRODUCT_MANUFACTURER,
    PRODUCT_NAME,
    "123456",
    PRODUCT_USB_CDC_NAME,
};

static void cdc0_usb_install(void)
{
    s_cdc0_rx_sem = xSemaphoreCreateBinary();
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.string       = s_usb_strings;
    tusb_cfg.descriptor.string_count = sizeof(s_usb_strings) / sizeof(s_usb_strings[0]);
    // The TinyUSB device task defaults to core 1 / priority 5 — but core 1 runs the
    // real-time audio pipeline at priority 9-12, which starves it under load and
    // freezes the CDC (USB stays enumerated, TX stops). Move it to core 0 and give
    // it headroom. tud_task() sleeps on its event queue when idle, so a higher
    // priority only costs CPU when there is actual USB work to service.
    // (Confirmed by the 2026-07 project-wide priority/affinity pass — see the
    // task map in radiosonic_main.c; verified under WiFi + console load.)
    tusb_cfg.task.xCoreID  = 0;    // off the audio core
    tusb_cfg.task.priority = 12;   // serviced promptly even during boot/WiFi bursts
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    const tinyusb_config_cdcacm_t acm0 = {
        .cdc_port                    = TINYUSB_CDC_ACM_0,
        .callback_rx                 = cdc0_rx_cb,
        .callback_line_state_changed = cdc0_line_state_changed,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm0));
    heap_checkpoint("usb-up");   // TinyUSB task + CDC TX FIFO (CONFIG_TINYUSB_CDC_TX_BUFSIZE) allocated
}

static int cdc0_vfs_open(const char *path, int flags, int mode)
{
    (void)path; (void)mode;
    int fd = cdc0_fd_clamp(s_cdc0_open_fd++);
    s_cdc0_flags[fd] = flags;
    return fd;
}
static int cdc0_vfs_close(int fd) { (void)fd; return 0; }
static int cdc0_vfs_fstat(int fd, struct stat *st)
{
    (void)fd;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}
static int cdc0_vfs_fcntl(int fd, int cmd, int arg)
{
    fd = cdc0_fd_clamp(fd);
    if (cmd == F_GETFL) return s_cdc0_flags[fd];
    if (cmd == F_SETFL) { s_cdc0_flags[fd] = arg; return 0; }
    errno = ENOSYS;
    return -1;
}
// Bulk write with \n -> \r\n expansion into a stack bounce buffer, then a single
// NON-BLOCKING flush. Per-byte queueing (the old path) issued a separate USB
// write+flush per character. A blocking flush (timeout > 0) here is hit by every
// ESP_LOG from every task and was originally suspected of stalling the port; the
// real freeze cause turned out to be TinyUSB-task starvation (see cdc0_usb_install),
// but a non-blocking flush is still correct: never wait on the EP from a caller's
// context. TinyUSB's device task drains the FIFO; queued bytes still go out.
static ssize_t cdc0_vfs_write(int fd, const void *src, size_t size)
{
    (void)fd;
    const uint8_t *p = (const uint8_t *)src;
    uint8_t buf[128];
    // BOUNDED take — console output must never be able to block the system.
    // Every ESP_LOG from every task (WiFi, httpd, sys_evt, TinyUSB itself)
    // funnels through here; with an infinite wait, one writer stalled inside
    // the TinyUSB FIFO (which has its own internal lock) can deadlock the
    // TinyUSB task against this mutex and take down USB + WiFi + all logging
    // while the rest of the system idles (observed 2026-07-02: board "hung",
    // USB enumerated but control transfers timing out, no WDT). Dropping a
    // log line after 100 ms is recoverable; a wedged system is not.
    if (xSemaphoreTake(s_cdc0_tx_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        return (ssize_t)size;   // output dropped — never block the caller
    }
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
        if (p[i] == '\n') buf[n++] = '\r';                 // CRLF for terminals
        buf[n++] = p[i];
        if (n >= sizeof(buf) - 1) {                        // flush chunk (room for a CR)
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf, n);
            n = 0;
        }
    }
    if (n) tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf, n);
    // Non-blocking flush (timeout 0): request the push but never wait. A blocking
    // flush here, hit by every ESP_LOG from every task, stalled callers on the USB
    // EP and dropped the port under log floods. TinyUSB's device task drains the
    // FIFO on its own; queued bytes still go out.
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    xSemaphoreGive(s_cdc0_tx_mtx);
    return (ssize_t)size;
}
static ssize_t cdc0_vfs_read(int fd, void *dst, size_t size)
{
    fd = cdc0_fd_clamp(fd);
    if (size == 0) return 0;
    uint8_t *out = (uint8_t *)dst;
    size_t got = 0;
    for (;;) {
        size_t rx = 0;
        tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, out, size, &rx);
        if (rx > 0) { got = rx; break; }
        if (s_cdc0_flags[fd] & O_NONBLOCK) { errno = EWOULDBLOCK; return -1; }  // probe poll
        // DTR 0->1 (re)connect: break the blocked edit so the loop re-probes/re-banners.
        // MUST return -1, NOT 0: linenoiseDumb() treats read<0 as a clean stop but on
        // read==0 it leaves its `char c='c'` initializer (or the last byte) unchanged and
        // stores/echoes that stale byte every iteration — a self-feeding 'c'/'n' flood that
        // fills the line buffer with no NUL, so linenoise()'s sanitize() then runs off the
        // end of the heap allocation and corrupts it (TLSF assert on the next malloc ->
        // PANIC). linenoiseEdit (smart mode) treats read<=0 identically, so -1 is safe there.
        if (s_reprobe) { s_dumb_line_len = 0; errno = EIO; return -1; }
        xSemaphoreTake(s_cdc0_rx_sem, pdMS_TO_TICKS(100));  // block until RX (or re-poll)
    }
    // CR -> LF, plus a dumb-mode line-length guard. linenoiseDumb() has no bounds
    // check past buflen, and linenoise()'s sanitize() then over-reads a completely
    // full (no-NUL) line buffer -> heap corruption (the panic this bug caused). Cap the
    // dumb-mode line one byte short by force-injecting a newline so the buffer can never
    // fill completely. Smart mode (linenoiseEdit) is bounds-safe and skipped; the probe
    // takes the O_NONBLOCK path above, so neither is affected.
    bool dumb = linenoiseIsDumbMode();
    for (size_t i = 0; i < got; i++) {
        if (out[i] == '\r') out[i] = '\n';                 // CR -> LF
        if (!dumb) continue;
        if (out[i] == '\n') { s_dumb_line_len = 0; }
        else if (++s_dumb_line_len >= s_console_max_line - 1) {
            out[i] = '\n';                                 // force-terminate before overflow
            s_dumb_line_len = 0;
        }
    }
    return (ssize_t)got;
}

// linenoise read seam. The edit loop and getCursorPosition() call read_func; we
// route it straight to our blocking VFS read (bypassing any newlib O_NONBLOCK
// quirk on the USB path). Overriding the weak linenoiseSetReadCharacteristics()
// keeps our read_func installed even after linenoiseProbe() resets the default.
static ssize_t cdc0_console_read(int fd, void *buf, size_t n)
{
    return cdc0_vfs_read(fd, buf, n);
}
void linenoiseSetReadCharacteristics(void)
{
    int f = fileno(stdin);
    int fl = fcntl(f, F_GETFL);
    fcntl(f, F_SETFL, fl & ~O_NONBLOCK);   // blocking by default (probe toggles it itself)
    linenoiseSetReadFunction(cdc0_console_read);
}

// ESP_LOG redraw guard: while the console is blocked in linenoise(), an async log
// from another task would inject bytes into the middle of the edited line. The
// hook emits CR + erase-line first so the log lands on its own clean line; the
// line editor redraws prompt + buffer on the next keystroke.
static int console_log_vprintf(const char *fmt, va_list ap)
{
    if (s_editing) fputs("\r\x1b[K", stdout);
    return vprintf(fmt, ap);
}

// Register the blocking CDC VFS and point stdin/stdout/stderr at it. With
// CONFIG_ESP_CONSOLE_NONE nothing else binds 0/1/2, so this is the single binder
// => stdin=0/stdout=1/stderr=2. Unbuffered both ways (control writes have no '\n').
static void cdc0_stdio_redirect(void)
{
    s_cdc0_tx_mtx = xSemaphoreCreateMutex();
    const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open  = cdc0_vfs_open,
        .close = cdc0_vfs_close,
        .read  = cdc0_vfs_read,
        .write = cdc0_vfs_write,
        .fstat = cdc0_vfs_fstat,
        .fcntl = cdc0_vfs_fcntl,
    };
    ESP_ERROR_CHECK(esp_vfs_register(CDC_CONSOLE_VFS_PATH, &vfs, NULL));
    freopen(CDC_CONSOLE_VFS_PATH, "r", stdin);
    freopen(CDC_CONSOLE_VFS_PATH, "w", stdout);
    freopen(CDC_CONSOLE_VFS_PATH, "w", stderr);
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    linenoiseSetReadFunction(cdc0_console_read);
    esp_log_set_vprintf(console_log_vprintf);
}

// Human-readable reset reason for the banner. With the primary console NONE,
// panic output is invisible — this is the first clue after any unexpected
// reboot (POWERON = supply collapsed; PANIC/WDT = firmware fault, coredump
// partition has the backtrace).
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB";
        case ESP_RST_JTAG:      return "JTAG";
        default:                return "UNKNOWN";
    }
}

// Probe for a smart terminal (answers ESC[5n) and print the banner. The user
// attaches the terminal while the board runs, so we probe at startup and on every
// reconnect (s_reprobe set by the DTR 0->1 transition). Never blocks: linenoiseProbe
// has its own ~500 ms timeout and a prompt is always shown regardless.
static void console_probe(void)
{
    for (int w = 0; !s_cdc0_dtr && w < 2500; w += 20) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_cdc0_dtr) vTaskDelay(pdMS_TO_TICKS(250));
    s_last_probe = linenoiseProbe();
    linenoiseSetDumbMode(s_last_probe != 0);
    s_reprobe = false;
    ESP_LOGI(CONSOLE_TAG, "probe=%d mode=%s dtr=%d fds(in/out/err)=%d/%d/%d",
             s_last_probe, linenoiseIsDumbMode() ? "dumb" : "smart", (int)s_cdc0_dtr,
             fileno(stdin), fileno(stdout), fileno(stderr));
    printf("\n" BANNER_TITLE "\n" BANNER_VERSION "  [last reset: %s]\n",
           reset_reason_str(esp_reset_reason()));
}

// --- Main Console Task ---
static void console_task(void *arg)
{
    (void)arg;
    cdc0_usb_install();      // TinyUSB + CDC0 (the shared USB PHY moves to USB-OTG here)
    cdc0_stdio_redirect();   // custom VFS + stdio redirect + linenoise read seam + log hook

    esp_console_config_t console_cfg = ESP_CONSOLE_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_init(&console_cfg));
    register_console_commands();

    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback *)&esp_console_get_hint);
    linenoiseHistorySetMaxLen(100);
    linenoiseSetMaxLineLen(console_cfg.max_cmdline_length);
    s_console_max_line = console_cfg.max_cmdline_length;   // for the dumb-mode over-read guard

    console_probe();   // smart/dumb auto-select + banner (re-runs on reconnect)

    while (1) {
        if (s_reprobe) console_probe();   // re-probe + re-banner on (re)connect
        s_editing = true;
        char *line = linenoise("dsp> ");
        s_editing = false;
        if (line == NULL) {            // EOF / reconnect — back off; loop re-probes
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (line[0] != '\0') {
            linenoiseHistoryAdd(line);
            int cmd_ret = 0;
            esp_err_t err = esp_console_run(line, &cmd_ret);
            if (err == ESP_ERR_NOT_FOUND) {
                printf("Unrecognized command\n");
            } else if (err == ESP_ERR_INVALID_ARG) {
                // command was empty — ignore
            } else if (err == ESP_OK && cmd_ret != ESP_OK) {
                printf("Command returned non-zero error code: 0x%x\n", cmd_ret);
            } else if (err != ESP_OK) {
                printf("Internal error: %s\n", esp_err_to_name(err));
            }
        }
        linenoiseFree(line);
    }
}

// USB tether state for power_sleep: mounted == a host enumerated us.
bool console_usb_attached(void) {
    return tud_mounted();
}

void start_console_task(void) {
    // Core 0 with the rest of the control plane — core 1 is reserved for the
    // real-time audio pipeline (unpinned tasks may migrate onto it).
    xTaskCreatePinnedToCore(console_task, "cli", 8192, NULL, 5, NULL, 0);
}