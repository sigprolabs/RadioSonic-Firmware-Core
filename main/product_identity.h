// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SigPro Labs, LLC
#pragma once

// Product identity: the names the firmware reports about itself on the
// console and over WiFi. RadioSonic and SigPro Labs are trademarks of
// SigPro Labs, LLC. Firmware built from a fork and given to anyone else must
// change these (see TRADEMARKS.md), together with:
//   - the USB descriptor strings in sdkconfig.defaults
//     (CONFIG_TINYUSB_DESC_MANUFACTURER_STRING, _PRODUCT_STRING, _CDC_STRING);
//   - the page title and heading in main/ui/web_page.html.
// USB IDs: the board enumerates with Espressif's vendor ID (0x303A) and the
// esp_tinyusb default product ID (the TinyUSB menuconfig defaults). Neither is
// assigned to SigPro Labs, LLC.

#define PRODUCT_NAME          "RadioSonic"
#define PRODUCT_MANUFACTURER  "SigPro Labs, LLC"

// Console banner, printed at connect, by `ver`, and after a reset.
#define PRODUCT_BANNER        PRODUCT_NAME " by " PRODUCT_MANUFACTURER

// softAP fallback network (wifi_manager.c). The password must be 8-63
// characters for WPA2.
#define PRODUCT_AP_SSID       "RadioSonic"
#define PRODUCT_AP_PASSWORD   "radiosonic"
