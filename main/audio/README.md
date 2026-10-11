<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# main/audio/ — the signal path

Everything that touches samples. Hot-path code here runs on core 1 at 48 kHz:
no blocking, no allocation, no logging inside the sample loop.

- `audio_codec.c/.h` — AIC3204 register driver over SPI: probe, init, gains,
  sleep/wake, TDM slot programming for up to 3 codecs.
- `audio_dsp.c/.h` — the framework: Q31/Q15↔float, L/R unpack, per-sample
  loop, output clipping, load meter, TDM per-pair dispatch + mixdown.
- `dsp_effects.c/.h`: the effect framework (registry, hooks, config lock,
  per-pair selection, meter) and bypass, the registry's one built-in entry.
  `dsp_effects_init()` registers channel, then calls `rs_user_effects_init()`
  for the sandbox. New effects go in `components/rs_student_fx/`; see
  [docs/ADDING_EFFECTS.md](../../docs/ADDING_EFFECTS.md).
- `channel_fx.c/.h`: the channel multi-tap delay, laid out like a sandbox
  effect. The file exports its record, `g_fx_channel`, and
  `channel_fx_init()`, and `dsp_effects_init()` registers it with
  `channel_fx_init(); dsp_effects_register(&g_fx_channel);`. It is the worked
  example in [docs/ADDING_EFFECTS.md](../../docs/ADDING_EFFECTS.md).
- `channel_panel.html`: the channel effect's web card, embedded by
  `main/CMakeLists.txt` and set as its `web_panel`. It is the worked example
  of an effect panel.
- `audio_store.c/.h` — recorder (1 MB PSRAM ring, STORAGE + LIVE cursors) and
  playback (512 KB ring), SD and flash backends, `sd_diag`.
- `wav_util.h` — the one canonical 44-byte WAV header.

May include `board/` (pins, HAL) — never `ui/`.
