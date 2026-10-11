<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# Getting Started

From a clean machine to a `dsp>` prompt. Works the same on Windows, macOS,
and Linux — only the serial-port names differ (`COMx` vs `/dev/tty*`).

## Prerequisites

- **ESP-IDF v5.5.x**, installed per Espressif's
  [Get Started guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/index.html)
  for your OS. On Windows, the ESP-IDF extension for VS Code and Espressif's
  installer (EIM) both work; the extension installs through EIM. Pick a
  v5.5.x release. The firmware is built and tested on v5.5, and `flash.cmd`
  stops if it finds another version. The `idf.py` commands below assume an
  IDF-activated shell (`export.sh` / `export.bat` / the IDF PowerShell, per
  that guide). `flash.cmd` on Windows activates ESP-IDF itself.
- A USB cable to the board's USB connector (it carries flashing, the console,
  and file transfer — no separate adapter needed).

## Clone and configure

```
git clone <repo-url> RadioSonicFirmware
cd RadioSonicFirmware
idf.py set-target esp32s3
idf.py menuconfig
```

In menuconfig, open **RadioSonic Board Configuration → PCB revision** and
select your board. The default is **Rev A**, the production board (silkscreen
SPL-RS-01-A). Rev 1, 2 and 3 (SPL-RS-01-03) are pre-production boards; owners
of those must change it. A wrong-revision build can boot but drive the wrong
pins (see `main/Kconfig.projbuild`); `ver` on the console prints which
revision the firmware was built for.

> **Note:** `sdkconfig` is untracked. `sdkconfig.defaults` is the source of
> truth for project-critical settings; any menuconfig change worth keeping
> belongs there too.

## Build

```
idf.py build
```

## Flash

**Fresh or non-responsive board:** hold the BOOT button while pressing RESET
to enter ROM download mode, then

```
idf.py -p <port> flash
```

**Board already running this firmware:** no buttons needed. On the console,
run

```
bootloader
```

The board reboots into ROM download mode and re-enumerates as the ESP32-S3
USB JTAG/serial device — a *different* serial port than the console. Flash
that port with `idf.py -p <port> flash`.

## Windows: build and flash with one command

From the repo folder, in PowerShell or Command Prompt:

```
.\flash.cmd
```

`flash.cmd` finds and activates ESP-IDF, builds, switches the board into
download mode, flashes it and waits for it to reboot. On a Rev A board it
replaces every step above from `idf.py set-target` on. Other board revisions
still need the `menuconfig` step first.

Options:

- `-Full` also writes the bootloader and partition table. Use it after
  changing `sdkconfig` or `partitions.csv`.
- `-Port COM7` picks the board when more than one is plugged in.
- `-Clean` and `-ResetConfig` are described at the top of `flash.ps1`.

`flash.cmd` runs `flash.ps1` for you. Windows refuses to run `.ps1` scripts
by default ("running scripts is disabled on this system") and also refuses
ones extracted from a downloaded zip. `flash.cmd` bypasses that policy for
the one run and changes no system setting.

It looks for ESP-IDF in this order and uses the first that applies:

1. The current terminal, if ESP-IDF is already activated in it.
2. `$env:IDF_EXPORT_PS1`, if set to an `export.ps1` or EIM activation script.
3. Espressif's installer (EIM), which the VS Code extension also uses. It
   records its installs in `C:\Espressif\tools\eim_idf.json`.
4. The older ESP-IDF Tools Installer (`C:\Espressif\Initialize-Idf.ps1`).

When several ESP-IDF versions are installed, it picks v5.5.x. If it finds
only other versions, it stops and lists them.

The first build compiles all of ESP-IDF and takes several minutes. Later
builds recompile only what changed.

## First connection

The console runs on the S3's USB-OTG controller (TinyUSB CDC), not the port
used for flashing. **A few seconds after boot the board re-enumerates as a
new serial port** — open that one in your terminal (PuTTY, screen, minicom,
…) with:

- **Baud:** anything (USB CDC ignores it; 115200 is the conventional choice)
- **Flow control:** None (required)

You should see the RadioSonic banner and a `dsp>` prompt. If the port seems
dead, close and reopen it — some terminals grab the port before enumeration
finishes.

## First commands

```
status                # DSP mode, input, gains, SD card, WiFi, load meter
mode channel          # multi-tap delay: the dry signal plus three reflections
channel L 1 300 0.6   # move the left first reflection to 300 ms, weight 0.6
help                  # every registered command with its usage
```

Talk near the board in `channel` mode to hear the reflections.
`mode bypass` returns to pass-through.

## Where next

- [MANUAL.md](MANUAL.md): the full CLI reference (effects, recording,
  playback, WiFi/web UI, settings, sleep).
- [ADDING_EFFECTS.md](ADDING_EFFECTS.md): write your own effect as one source
  file in `components/rs_student_fx/` plus four registration edits, with the
  `channel` effect as the worked example.
- [ARCHITECTURE.md](ARCHITECTURE.md): how the firmware is put together.
