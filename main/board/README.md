<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# main/board/ — hardware-specific code

Pins, HAL implementations, and board services. The Kconfig revision choice
selects which `hal_rev*` header compiles; production boards are Rev A.

- `hal.h` — the HAL API every revision implements + the revision selector.
- `hal_rev1.h/.c` — Rev 1 prototype (SDSPI SD, no panel buttons/LEDs).
- `hal_rev2.h/.c` — Rev 2 pins + the shared implementation for Rev 2, 3 and A.
- `hal_rev3.h` — Rev 3 (DVT) pin deltas over Rev 2 (see its delta list).
- `hal_reva.h` — Rev A (production) pins; same ESP32 pinout as Rev 3, plus the
  list of what Rev A changed on the board.
- `rgb_leds.c/.h` — SK6805 4-LED strip driver (RMT).
- `sd_storage.c/.h` — SD mount + `sd_*` CLI commands.
- `power_sleep.c/.h` — light-sleep enter/wake sequencing.

Dependencies point down to ESP-IDF, with documented exceptions that read or
call upward: the LED status render reads `g_state`/the DSP meter for display,
and `power_sleep`/`sd_storage` are board-adjacent services that orchestrate
`audio/` and `ui/` modules.
