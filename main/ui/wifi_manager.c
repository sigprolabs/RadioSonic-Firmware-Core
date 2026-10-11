// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
// wifi_manager.c — WiFi lifecycle for the RadioSonic control surfaces.
//
// One job: get the radio up and keep it healthy. That means NVS-stored STA
// credentials with softAP fallback, the brownout mitigation this board's
// marginal USB supply demanded (boot settle delay, TX-power cap, association-
// retry backoff), the `wifi` CLI command, and the light-sleep suspend/resume
// pair. The HTTP control panel itself lives in web_server.c: this module
// drives its lifecycle (web_server_start()/web_server_stop() once the radio
// is up, web_server_stop() on suspend and `wifi off`); web_server never calls
// back in except wifi_manager_get_wifi_status() (read-only, for /api/status)
// and the credential-save helpers behind /api/wifi/*.

#include "wifi_manager.h"
#include "web_server.h"
#include "audio_pipeline.h"   // heap_checkpoint()
#include "product_identity.h" // softAP SSID and password

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_console.h"

static const char *TAG = "wifi";

#define NVS_NS              "radiosonic"
// Compiled-in STA fallback — EMPTY by default (an empty SSID skips straight
// to AP mode; see the DEFAULT_WIFI_SSID[0] guard in wifi_task). Set these only
// in a local build; never commit real credentials.
#define DEFAULT_WIFI_SSID   ""
#define DEFAULT_WIFI_PASS   ""
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRIES    3
#define WIFI_TIMEOUT_MS     12000

// --- Brownout mitigation -----------------------------------------------------
// WiFi TX bursts at the IDF-default 20 dBm collapsed a marginal USB supply
// (association-retry storm -> POWERON reset loop). Three
// levers, applied together:
//   1. Boot settle delay before the radio's first calibration/association burst
//      stacks on top of USB-enumeration + SD + codec inrush.
//   2. Runtime TX-power cap (NVS "wifi_txpow", `wifi power` CLI). Units are
//      0.25 dBm; the PHY build ceiling is CONFIG_ESP_PHY_MAX_WIFI_TX_POWER.
//   3. Backoff between association retries (see wifi_event_handler) so failed
//      attempts don't hammer the supply back-to-back.
#define WIFI_BOOT_SETTLE_MS       2000
#define WIFI_TXPOW_DEFAULT_QDBM   34   // 8.5 dBm — reliable on a 500 mA USB port
#define WIFI_TXPOW_MIN_DBM        2
#define WIFI_TXPOW_MAX_DBM        13   // keep equal to CONFIG_ESP_PHY_MAX_WIFI_TX_POWER
#define WIFI_RETRY_BACKOFF_US     500000ULL  // 0.5 s, doubled each retry

static EventGroupHandle_t s_wifi_eg;
static int   s_retry        = 0;
static char  s_ip[24]       = "192.168.4.1";
static bool  s_is_ap        = true;
static bool  s_wifi_disabled = false;
static bool  s_suspended    = false;  // light-sleep suspend in effect
// "STA has associated at least once this boot". Distinct from
// web_server_started(): since httpd now starts BEFORE the radio (see
// wifi_task), httpd being up no longer implies WiFi ever came up, and the
// disconnect handler must not mistake a failed FIRST association for a lost
// link — that would swallow the AP fallback.
static bool  s_sta_connected_once = false;
static esp_timer_handle_t s_reconnect_tmr = NULL;
// The old s_wifi_ready flag ("WiFi up + httpd started, wifi_task done") is
// web_server_started() now — httpd start success is web_server.c's to know
// (it can arrive late, off the heap-gate retry timer), and the sticky getter
// preserves the exact gating behavior suspend() and the reconnect cadence had.

// ==========================================================
// NVS helpers
// ==========================================================

static bool load_wifi_creds(char *ssid, char *pass, bool *force_ap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = 33, pl = 65;
    bool ok = (nvs_get_str(h, "wifi_ssid", ssid, &sl) == ESP_OK) &&
              (nvs_get_str(h, "wifi_pass", pass, &pl) == ESP_OK);
    uint8_t ap = 0;
    nvs_get_u8(h, "wifi_force_ap", &ap);
    *force_ap = (ap != 0);
    nvs_close(h);
    return ok && !(*force_ap) && (ssid[0] != '\0');
}

void wifi_manager_save_sta_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "wifi_ssid", ssid);
    nvs_set_str(h, "wifi_pass", pass);
    nvs_set_u8(h, "wifi_force_ap", 0);
    nvs_commit(h);
    nvs_close(h);
}

void wifi_manager_save_force_ap(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "wifi_force_ap", 1);
    nvs_commit(h);
    nvs_close(h);
}

static void save_wifi_disabled(bool disabled)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "wifi_disabled", disabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// ==========================================================
// TX-power cap (brownout mitigation)
// ==========================================================

static int8_t load_wifi_txpow_qdbm(void)
{
    int8_t q = WIFI_TXPOW_DEFAULT_QDBM;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int8_t v;
        if (nvs_get_i8(h, "wifi_txpow", &v) == ESP_OK &&
            v >= WIFI_TXPOW_MIN_DBM * 4 && v <= WIFI_TXPOW_MAX_DBM * 4) {
            q = v;
        }
        nvs_close(h);
    }
    return q;
}

static void save_wifi_txpow_qdbm(int8_t qdbm)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "wifi_txpow", qdbm);
    nvs_commit(h);
    nvs_close(h);
}

// Must run after every esp_wifi_start() — both settings reset on each start.
static void apply_wifi_txpow(void)
{
    int8_t q = load_wifi_txpow_qdbm();
    esp_err_t err = esp_wifi_set_max_tx_power(q);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi TX power capped at %.2f dBm", q * 0.25);
    } else {
        ESP_LOGW(TAG, "esp_wifi_set_max_tx_power(%d): %s", q, esp_err_to_name(err));
    }

    // Modem sleep OFF. IDF's default is WIFI_PS_MIN_MODEM, which parks the
    // radio between the AP's DTIM beacons: measured 2026-09-06 it put the
    // board 218 ms away on a LAN whose gateway answers in 3 ms, and RTT is a
    // hard multiplier on TCP throughput (window / RTT), so it — not the TCP
    // window — was what held HTTP transfers near 78 KB/s. This board is
    // USB-powered and its consumers are the codec, MICBIAS and SD, so trading
    // ~30-50 mA of idle radio draw for a responsive control surface is the
    // right side of the ~500 mA budget (docs/ARCHITECTURE.md). Light sleep is
    // unaffected: wifi_manager_suspend() stops WiFi outright before sleeping.
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE): %s", esp_err_to_name(err));
    }
}

// ==========================================================
// WiFi event handler
// ==========================================================

// Deferred esp_wifi_connect() — runs in the esp_timer task, spacing retries
// out so back-to-back association bursts can't stack on the supply.
static void wifi_reconnect_cb(void *arg)
{
    if (!s_suspended) esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_suspended) {
            // esp_wifi_stop() during light-sleep suspend fires this event —
            // don't fight the suspend with reconnect attempts.
        } else if (s_retry < WIFI_MAX_RETRIES) {
            uint64_t delay_us = WIFI_RETRY_BACKOFF_US << s_retry;  // 0.5s, 1s, 2s
            s_retry++;
            ESP_LOGI(TAG, "WiFi retry %d/%d in %u ms", s_retry, WIFI_MAX_RETRIES,
                     (unsigned)(delay_us / 1000));
            esp_timer_stop(s_reconnect_tmr);  // no-op if not running
            esp_timer_start_once(s_reconnect_tmr, delay_us);
        } else if (s_sta_connected_once && !s_is_ap) {
            // Link lost after successful bring-up (wifi_task is gone, so the
            // FAIL bit has no listener). Keep retrying at a gentle cadence.
            esp_timer_stop(s_reconnect_tmr);
            esp_timer_start_once(s_reconnect_tmr, 10 * 1000 * 1000ULL);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_is_ap = false;
        s_retry = 0;   // fresh retry budget for the next link loss
        s_sta_connected_once = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

// ==========================================================
// Light-sleep suspend / resume
// ==========================================================

bool wifi_manager_suspend(void)
{
    if (s_wifi_disabled || !web_server_started() || s_suspended) return false;
    s_suspended = true;
    if (s_reconnect_tmr) esp_timer_stop(s_reconnect_tmr);
    web_server_stop();   // httpd + its retry timer; also resets the retry budget
    esp_wifi_stop();   // no deinit, no NVS write — config retained for resume
    ESP_LOGI(TAG, "WiFi suspended for sleep");
    return true;
}

void wifi_manager_resume(void)
{
    if (!s_suspended) return;
    s_retry = 0;
    s_suspended = false;
    esp_wifi_start();  // STA: STA_START event reconnects; AP: beacons resume
    apply_wifi_txpow();
    // web_server_stop() at suspend reset the httpd retry budget, so this start
    // gets fresh attempts — boot-time exhaustion doesn't carry over.
    web_server_start();
    ESP_LOGI(TAG, "WiFi resumed after sleep");
}

// ==========================================================
// WiFi task (STA -> AP fallback)
// ==========================================================

static void wifi_task(void *arg)
{
    // Brownout mitigation: let USB enumeration / SD / codec inrush settle
    // before the radio's first calibration + association bursts hit the supply.
    vTaskDelay(pdMS_TO_TICKS(WIFI_BOOT_SETTLE_MS));

    // NVS init — may already be done; both outcomes are acceptable
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Netif / event loop — tolerate "already initialised"
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ret);
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ret);

    // Check disabled flag before initializing WiFi hardware
    {
        uint8_t dis = 0;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "wifi_disabled", &dis);
            nvs_close(h);
        }
        if (dis) {
            ESP_LOGI(TAG, "WiFi disabled by user — skipping init");
            s_wifi_disabled = true;
            vTaskDelete(NULL);
            return;
        }
    }

    // ---- httpd FIRST, radio second (measured 2026-09-06) --------------------
    // httpd's task stack is one contiguous 16 KB internal block. The WiFi
    // driver's pools consume ~64 KB of internal RAM at esp_wifi_init/start and
    // leave the remainder in fragments — measured largest free block 8192 B
    // afterwards, in BOTH AP and STA, so httpd could never start once the
    // radio was up. Nothing in httpd_start needs the radio: lwIP is already
    // initialised above, and binding INADDR_ANY:80 succeeds with no netif —
    // connections simply arrive later, once an interface has an address.
    // Starting here costs nothing and takes the block while the heap is still
    // whole. (An earlier attempt reserved this at the top of app_main and
    // boot-looped: that starved SD/I2S/USB init, which all allocate after it.
    // Here every other boot allocation is already done and only WiFi follows.)
    heap_checkpoint("pre-wifi");
    web_server_start();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    const esp_timer_create_args_t tmr_args = {
        .callback = wifi_reconnect_cb,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tmr_args, &s_reconnect_tmr));

    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    char ssid[33] = {0}, pass[65] = {0};
    bool force_ap = false;
    bool try_sta = load_wifi_creds(ssid, pass, &force_ap);
    if (!try_sta && DEFAULT_WIFI_SSID[0]) {
        strlcpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid));
        strlcpy(pass, DEFAULT_WIFI_PASS, sizeof(pass));
        try_sta = true;
    }

    if (try_sta) {
        ESP_LOGI(TAG, "Trying STA mode: SSID=%s", ssid);
        esp_netif_create_default_wifi_sta();

        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        apply_wifi_txpow();
        heap_checkpoint("wifi-up");   // boot-time internal-heap peak

        EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "STA connected, IP: %s", s_ip);
            web_server_start();
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGW(TAG, "STA failed, falling back to AP mode");
        ESP_ERROR_CHECK(esp_wifi_stop());
    }

    // softAP fallback
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = PRODUCT_AP_SSID,
            .ssid_len       = sizeof(PRODUCT_AP_SSID) - 1,
            .password       = PRODUCT_AP_PASSWORD,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        }
    };
    snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
    s_is_ap = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_wifi_txpow();
    heap_checkpoint("wifi-up");   // boot-time internal-heap peak

    web_server_start();           // no-op if the early start already succeeded
    vTaskDelete(NULL);
}

// ==========================================================
// WiFi CLI command
// ==========================================================

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        if (s_wifi_disabled) {
            printf("WiFi: disabled\n");
        } else {
            printf("WiFi: %s\nIP:   %s\nURL:  http://%s\n",
                   s_is_ap ? "AP (" PRODUCT_AP_SSID " / " PRODUCT_AP_PASSWORD ")"
                           : "STA (DHCP)",
                   s_ip, s_ip);
        }
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        save_wifi_disabled(true);
        // Stop the timers BEFORE tearing down (mirror of wifi_manager_suspend):
        // a pending reconnect/retry callback firing after deinit would call
        // esp_wifi_connect()/httpd_start on a dead stack.
        if (s_reconnect_tmr)  esp_timer_stop(s_reconnect_tmr);
        web_server_stop();   // stops the httpd retry timer, then httpd itself
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_disabled = true;
        printf("WiFi disabled. Use 'wifi on' to re-enable.\n");
    } else if (argc == 2 && strcmp(argv[1], "on") == 0) {
        save_wifi_disabled(false);
        printf("WiFi will be enabled after reboot. Rebooting in 3 seconds...\n");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else if (argc == 2 && strcmp(argv[1], "ap") == 0) {
        wifi_manager_save_force_ap();
        printf("Saved: force AP mode. Restarting...\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if ((argc == 2 || argc == 3) && strcmp(argv[1], "power") == 0) {
        if (argc == 2) {
            int8_t q = load_wifi_txpow_qdbm();
            printf("WiFi TX power cap: %.2f dBm (build ceiling %d dBm)\n",
                   q * 0.25, WIFI_TXPOW_MAX_DBM);
            if (!s_wifi_disabled && web_server_started()) {
                int8_t cur = 0;
                if (esp_wifi_get_max_tx_power(&cur) == ESP_OK)
                    printf("Currently active: %.2f dBm\n", cur * 0.25);
            }
        } else {
            char *end;
            double dbm = strtod(argv[2], &end);
            if (*end != '\0' || dbm < WIFI_TXPOW_MIN_DBM || dbm > WIFI_TXPOW_MAX_DBM) {
                printf("Power must be %d..%d dBm (0.25 dB steps)\n",
                       WIFI_TXPOW_MIN_DBM, WIFI_TXPOW_MAX_DBM);
                return 1;
            }
            int8_t q = (int8_t)(dbm * 4.0 + 0.5);
            save_wifi_txpow_qdbm(q);
            printf("WiFi TX power cap set to %.2f dBm (persisted)\n", q * 0.25);
            if (!s_wifi_disabled && !s_suspended) apply_wifi_txpow();
        }
    } else if (argc == 3) {
        wifi_manager_save_sta_creds(argv[1], argv[2]);
        printf("Saved SSID: %s. Restarting...\n", argv[1]);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        printf("Usage:\n"
               "  wifi status               — current mode and IP\n"
               "  wifi off                  — disable WiFi immediately (persists across reboot)\n"
               "  wifi on                   — re-enable WiFi (reboots in 3 s)\n"
               "  wifi <ssid> <password>    — save credentials, reconnect\n"
               "  wifi ap                   — force AP mode\n"
               "  wifi power [dbm]          — show/set TX power cap (%d..%d dBm, persisted;\n"
               "                              lower = less supply droop, shorter range)\n",
               WIFI_TXPOW_MIN_DBM, WIFI_TXPOW_MAX_DBM);
    }
    return 0;
}

void register_wifi_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi",
        .help    = "wifi status | wifi off | wifi on | wifi ap | wifi power [dbm] | wifi <ssid> <password>",
        .func    = cmd_wifi,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

// ==========================================================
// Entry point
// ==========================================================

void wifi_manager_get_wifi_status(bool *disabled, bool *is_ap, char *ip_buf, size_t ip_buf_len)
{
    *disabled = s_wifi_disabled;
    *is_ap    = s_is_ap;
    strlcpy(ip_buf, s_ip, ip_buf_len);
}

void wifi_manager_init(void)
{
    xTaskCreatePinnedToCore(wifi_task, "wifi", 8192, NULL, 3, NULL, 0);
}
