<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# RadioSonic

Firmware for the RadioSonic™ board, an ESP32-S3 audio effects processor and
DSP teaching platform by SigPro Labs™. Hardware designed by Jamie Mitchell;
DSP algorithms developed with Dan Boschen. The board pairs a TI AIC3204 codec
(48 kHz, stereo, 32-bit) with the S3's two cores: one core runs the real-time
signal path and the other runs everything else.

<!-- Board photo placeholder: add an image of the Rev A board here, e.g.
     ![RadioSonic Rev A](docs/img/radiosonic-reva.jpg) -->

## Features

- Real-time DSP effect registry. Each effect is one source file holding its
  per-sample math and a `dsp_effect_t` record, registered with
  `dsp_effects_register()`. The console, web UI, LEDs, panel buttons and
  settings pick it up automatically
  ([docs/ADDING_EFFECTS.md](docs/ADDING_EFFECTS.md)). The firmware ships with
  bypass and a multi-tap delay and reverb (`channel`). The course modules
  used in SigPro Labs courses plug in through the same interface.
- Recording and playback. Streaming WAV capture (before or after the DSP) to
  an SD card or a flash partition, and playback mixed into the output, built
  on PSRAM rings that keep all storage I/O off the audio core.
- Three control surfaces: the USB console, a WiFi web UI with an HTTP API,
  and the front-panel buttons and pot. All of them change one shared state.
- TDM expansion: up to 3 codecs (6 input channels) on one I2S bus, with
  independent DSP state per codec pair and a stereo mixdown.

## Hardware revisions

The production board is Rev A. Earlier revisions were pre-production
prototypes. The firmware supports all four through a menuconfig choice
(default: Rev A).

| Public name | Firmware option (`menuconfig`) | Board silkscreen | Status |
|---|---|---|---|
| Rev A | `CONFIG_BOARD_REVISION_REVA` | SPL-RS-01-A | production |
| Rev 3 | `CONFIG_BOARD_REVISION_REV3` | SPL-RS-01-03 | pre-production (DVT) |
| Rev 2 | `CONFIG_BOARD_REVISION_REV2` | none | pre-production prototype |
| Rev 1 | `CONFIG_BOARD_REVISION_REV1` | none | pre-production prototype |

[docs/hardware/](docs/hardware/) has the Rev A netlist and the pin table for
every revision.

## Quick start

Build with ESP-IDF v5.5.x, select your board revision, flash, and connect to
the USB console. [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) walks
through each step.

## Documentation

| Doc | What it covers |
|---|---|
| [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) | prerequisites, build, flash, first connection |
| [docs/MANUAL.md](docs/MANUAL.md) | every console command, the web UI, buttons, settings |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | data path, task map, memory split, module map |
| [docs/ADDING_EFFECTS.md](docs/ADDING_EFFECTS.md) | writing your own effect, with a worked example |
| [docs/SERIAL_XFER.md](docs/SERIAL_XFER.md) | USB file-transfer wire protocol (`xfer`) |
| [docs/TINYUSB_CONSOLE.md](docs/TINYUSB_CONSOLE.md) | how the console runs over TinyUSB CDC |

## Course context

This firmware doubles as lab code for a hands-on DSP course. The framework
handles the real-time plumbing, so a student can implement a filter and hear
it by writing one function that processes a single stereo sample.

## License

Copyright (c) 2026 SigPro Labs, LLC.

The firmware in this repository is open source under the MIT License. See
`LICENSES/MIT.txt`. Each file states its license in an SPDX header on its
first lines.

The course modules used in SigPro Labs courses are not part of this repository
and are not open source. They are provided separately to course participants
under their own license.

RadioSonic and SigPro Labs are trademarks of SigPro Labs, LLC. The license
grants no rights to use either trademark. See `TRADEMARKS.md`.

The board netlist in `docs/hardware/` is hardware design data, licensed under
the CERN-OHL-P v2 (`LICENSES/CERN-OHL-P-2.0.txt`), as its `.license` file
states.

Third-party software the firmware builds on, and its licenses, is listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
