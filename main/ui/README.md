<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# main/ui/ — control surfaces

Every way a human changes the box. All of them converge on `g_state`
(multi-field DSP configs go through `dsp_cfg_lock`); none touch samples.

- `cli_console.c/.h` — the console: esp_console REPL over TinyUSB CDC, plus
  core commands (`status`, `mode`, `gain`, `bootloader`, …).
- `web_server.c/.h` — HTTP control panel and REST-ish API (heap-gated httpd
  start/retry); its lifecycle is driven by `wifi_manager.c`.
- `web_page.html`: the control page, embedded at build time. It holds no
  effect code. At load it reads `GET /api/modes` and builds a card for each
  effect that has its own `web_panel` fragment or a text config (a generic
  card over `GET/POST /api/fx/<name>`). An effect with neither, such as
  bypass, gets no card.
- `wifi_manager.c/.h` — WiFi lifecycle: NVS creds, STA→AP fallback, brownout
  mitigation, `wifi` CLI command, light-sleep suspend/resume.
- `control_buttons.c/.h` — front-panel buttons: shift layer, feature/sub-mode
  select, gain steps. Addressed positionally `btn1..btn5` — the silkscreen
  refdes were renumbered between revs, positions weren't.
- `control_pot.c/.h`: analog pot (Rev 3 / Rev A), off by default. `pot volume`
  drives the DAC volume and `pot fx` the current effect's `pot_set`, with soft
  takeover through a dead-band.
- `led_status.c/.h`: what the 4-LED strip shows (input, bypass, the Mode LED
  in the current effect's `led_rgb`, sub-mode, the meter needle).
- `serial_xfer.c/.h` — `xfer` USB file transfer, flow-controlled and
  CRC-checked ([docs/SERIAL_XFER.md](../../docs/SERIAL_XFER.md)).
- `settings_store.c/.h` — NVS snapshot of all button/CLI-changeable state.

May call into `audio/` and `board/`; nothing depends on `ui/`.
