<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# RadioSonic effect component template

A starting point for writing your own DSP effect for the RadioSonic board as an ESP-IDF component. As shipped it compiles and registers no effect, so the firmware runs with the two effects in its core, bypass and channel, until you add yours.

## Files

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Registers this folder as an ESP-IDF component. It depends on `main` for `dsp_effects.h` and `common.h` and links with `WHOLE_ARCHIVE` so the registration hook is kept. |
| `my_filters.c` | `rs_user_effects_init()`, the hook the firmware calls at boot, and a commented placeholder effect: its `process` function, an init function and its record. |

## The worked example

The firmware's `channel` effect, a stereo multi-tap delay, is written and registered the way your effect will be, and it implements every hook in the effect record except `alias`. Read it alongside this template:

- `main/audio/channel_fx.c`: the `fx_channel*` hook functions, `channel_fx_init()`, which allocates the delay rings at boot, and the record `g_fx_channel`. `main/audio/channel_fx.h` declares the record and the init function.
- `main/audio/dsp_effects.c`: `dsp_effects_init()` registers channel with `channel_fx_init(); dsp_effects_register(&g_fx_channel);`, the same two calls `rs_user_effects_init()` in `my_filters.c` makes for your effect.
- `main/audio/channel_panel.html`: its web card, embedded by `main/CMakeLists.txt`.

`docs/ADDING_EFFECTS.md` in the firmware is the full contract: float ranges, the 48 kHz real-time budget, thread safety, per-pair state and the web panel rules.

## Build

The firmware may contain only one definition of `rs_user_effects_init()`. If your checkout has `components/rs_student_fx/`, remove it before adding this component, or write your effect in that component instead as `docs/ADDING_EFFECTS.md` describes.

1. Copy this folder into the firmware checkout under `components/`, for example as `components/my_effects/`. ESP-IDF builds every component it finds there.
2. Build and flash as `docs/GETTING_STARTED.md` describes: `idf.py build`, then `idf.py -p <port> flash` with the board in download mode (hold BOOT while pressing RESET, or run `bootloader` on the console). On Windows, `.\flash.ps1` from PowerShell does the build, the switch to download mode and the flash in one step.

## Add your effect

1. In `my_filters.c`, uncomment the placeholder block and rename `myeffect` throughout. The effect name is lowercase with no spaces, at most 15 characters, and must differ from every existing effect and console command.
2. Write the per-sample math in `fx_myeffect()`. For each optional hook you need, model it on channel's matching function in `main/audio/channel_fx.c`. `my_filters.c` already includes `common.h` for `FS_HZ` and `AUDIO_MAX_PAIRS` and the C and ESP-IDF headers `channel_fx.c` uses.
3. If the effect keeps per-pair buffers, allocate them in `myeffect_init()` as `channel_fx_init()` does. Otherwise delete `myeffect_init()`.
4. In `rs_user_effects_init()`, uncomment the `dsp_effects_register()` line, and the `myeffect_init()` line above it if you kept that function. Up to 8 effects can register: `DSP_USER_EFFECT_SLOTS` is 9, and channel takes one.
5. For a web card, save an HTML fragment next to the source (for example `my_panel.html`) and list it under `EMBED_TXTFILES` in `CMakeLists.txt`. Then declare `extern const char _binary_my_panel_html_start[];` and set `.web_panel = _binary_my_panel_html_start`. The symbol is `_binary_`, then the file name with every character that is not a letter or a digit replaced by `_`, then `_start`.
6. Add every additional source file to `SRCS` and to the `-O2` list in `CMakeLists.txt`.
7. Rebuild and flash, then check `mode <name>`, `status`, the web card, and a `settings save` / `settings load` round trip.
