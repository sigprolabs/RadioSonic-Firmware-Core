// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once

#include <stdbool.h>
#include <stddef.h>

// WiFi lifecycle manager. One job: get the radio up and keep it healthy —
// NVS-stored STA credentials with softAP fallback, the brownout mitigation a
// marginal USB supply demanded (boot settle delay, TX-power cap, association-
// retry backoff), the `wifi` CLI command, and the light-sleep suspend/resume
// pair. The HTTP control panel lives in web_server.c: this module drives its
// lifecycle (web_server_start()/web_server_stop()); web_server never calls
// back in except wifi_manager_get_wifi_status() (read-only, for /api/status)
// and the credential-save helpers behind /api/wifi/*.

// Initialize WiFi (STA via DHCP if credentials in NVS, else AP fallback)
// and start the HTTP control panel. Runs in a background task on Core 0.
// Call once in app_main after dsp_init().
void wifi_manager_init(void);

// Register the `wifi` CLI command (status / <ssid> <pass> / ap).
// Call in app_main alongside other register_*_cmd() calls.
void register_wifi_cmd(void);

// Return current WiFi state for display in the system status command.
void wifi_manager_get_wifi_status(bool *disabled, bool *is_ap, char *ip_buf, size_t ip_buf_len);

// Light-sleep support: stop httpd + WiFi without deinit or NVS writes, and
// the matching restart (driver retains STA/AP mode and config).
// suspend() returns false if WiFi was already off/not started — in that case
// do not call resume(), so the user's `wifi off` state survives sleep.
bool wifi_manager_suspend(void);
void wifi_manager_resume(void);

// Persist STA credentials / the force-AP flag to NVS. Shared by the `wifi`
// CLI command here and web_server.c's /api/wifi/sta and /api/wifi/ap
// handlers (both restart the board afterwards so wifi_task re-reads them).
void wifi_manager_save_sta_creds(const char *ssid, const char *pass);
void wifi_manager_save_force_ap(void);
