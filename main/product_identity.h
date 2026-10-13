// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once

// Product identity: every name the firmware reports about itself, on the
// console, over USB and on the web page. RadioSonic and SigPro Labs are
// trademarks of SigPro Labs, LLC. Firmware built from a fork and given to
// anyone else must change these (see TRADEMARKS.md).
//
// USB IDs: the board enumerates with Espressif's vendor ID (0x303A) and the
// esp_tinyusb default product ID (the TinyUSB menuconfig defaults). Neither is
// assigned to SigPro Labs, LLC.

#define PRODUCT_NAME          "RadioSonic"
#define PRODUCT_MANUFACTURER  "SigPro Labs, LLC"

// Console banner, printed at connect, by `ver`, and after a reset.
#define PRODUCT_BANNER        PRODUCT_NAME " by " PRODUCT_MANUFACTURER

// USB string descriptors (cli_console.c). The device also reports
// PRODUCT_MANUFACTURER and PRODUCT_NAME. USB strings are cut at 31 characters.
#define PRODUCT_USB_CDC_NAME  PRODUCT_NAME " Console"

// softAP fallback network (wifi_manager.c). The password must be 8-63
// characters for WPA2.
#define PRODUCT_AP_SSID       "RadioSonic"
#define PRODUCT_AP_PASSWORD   "radiosonic"

// The web page (main/ui/web_page.html) shows PRODUCT_NAME and PRODUCT_AP_SSID
// through placeholders that web_server.c fills in as it serves the page.
