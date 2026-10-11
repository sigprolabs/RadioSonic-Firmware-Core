<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# Third-party software

The firmware builds on the software below. None of it is copied into this
repository: ESP-IDF is installed separately, and the IDF Component Manager
downloads the managed components into `managed_components/` at build time,
at the versions pinned in `dependencies.lock`. Each keeps its own license.

## Firmware

| Component | Used for | License |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) v5.5 (Espressif Systems) | Framework, drivers, FreeRTOS, lwIP, FATFS, WiFi | Apache-2.0, with the licenses of its own third-party parts listed in its repository |
| [espressif/esp_tinyusb](https://components.espressif.com/components/espressif/esp_tinyusb) | USB CDC console and file transfer | Apache-2.0 |
| [espressif/tinyusb](https://components.espressif.com/components/espressif/tinyusb) (TinyUSB, hathach) | USB device stack, pulled in by esp_tinyusb | MIT |

## Host scripts

| Package | Used by | License |
|---|---|---|
| [pyserial](https://github.com/pyserial/pyserial) | `scripts/rs_cli.py` | BSD-3-Clause |
| [Python-Markdown](https://github.com/Python-Markdown/markdown) | `scripts/build_manual_pdf.py` | BSD-3-Clause |
