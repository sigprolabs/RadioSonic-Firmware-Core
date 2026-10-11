<!-- SPDX-License-Identifier: CERN-OHL-P-2.0 -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# Hardware reference files

Board-level reference material for readers who want to trace signals beyond
what the firmware headers document (`main/board/hal_rev*.h` remains the
authoritative pin map the code actually uses).

| File | What it is |
|------|------------|
| `RadioSonic_Netlist.net` | KiCad netlist of the Rev A (SPL-RS-01-A) board: full connectivity for tracing any net. Exported from hardware repo commit 0b041fd (Rev A as-built) |
| `Pinouts.csv` | ESP32-S3 pin assignment table across board revisions |

Board naming: production boards are **Rev A** = firmware `CONFIG_BOARD_REVISION_REVA`
= silkscreen `SPL-RS-01-A`. Rev 3 (`CONFIG_BOARD_REVISION_REV3`, silkscreen
`SPL-RS-01-03`) has the same ESP32 pinout; Rev 1/2/3 were pre-production boards.

Everything in this folder is hardware design data, licensed under the
CERN-OHL-P v2 (`../../LICENSES/CERN-OHL-P-2.0.txt`).
