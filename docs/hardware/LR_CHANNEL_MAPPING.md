<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# Left/right channel mapping: what is swapped, why, and what compensates

Status 2026-10-01. Everything L/R known so far, in one place. Earlier explanations that turned
out wrong are kept at the end, labelled as superseded.

## Short answer

- **One physical cause, most likely:** the audio jack footprint. All three jacks (J1 line in, J2
  headphone, J3 line out), on every revision since Nov 2025, are XKB PJ-3200 (LCSC C2689690) on
  the easyeda2kicad footprint `AUDIO-TH_PJ-3200`. That footprint puts the plug **tip** (the
  *left* signal) on pad 4, and the schematic calls pad 4 **R**. So every jack has tip and ring
  swapped relative to the schematic.
  - Status: consistent with every measurement to date, and with the footprint geometry.
  - Not yet checked against the PJ-3200 datasheet.
  - A 30-second continuity test settles it; see below.
- **The mics don't go through a jack**, so they are straight.
- **Two firmware compensations follow from that one cause**, because the swapped jacks sit on
  both sides of the codec:
  1. **Outputs:** the DAC cross-map, `P0 R63 = 0xE8`, in place since 2026-06-11. It puts
     headphone and line out right for every source.
  2. **Line input:** a left/right swap of the line-in samples in `rx_fill`
     (`LINE_IN_LR_SWAPPED` in `hal_rev3.h` / `hal_reva.h`), added 2026-10-01 for issue #20.
     It is committed but not yet run on a board.
- **What each one fixes:**
  - Without #1, the mics come out reversed (that was issue #1).
  - With #1 and without #2, line in comes out reversed (that is issue #20).
  - Fixing the footprint on the next PCB revision removes the need for both.

## Every path, and what each configuration does

"Physical swaps" counts the swapped jacks a path goes through.

| Path | Physical swaps | No compensation | R63 cross only (shipped until 2026-10-01) | R63 cross + line-in swap |
|---|---|---|---|---|
| Mic → headphone / line out | 1 (output jack) | reversed | correct | correct |
| Line in → headphone / line out | 2 (input + output jack) | correct (the two cancel) | **reversed (issue #20)** | correct |
| Mic → recording / `xfer live` / effects (data) | 0 | correct | correct | correct |
| Line in → recording / `xfer live` (data) | 1 (input jack) | reversed | reversed | correct |
| SD playback / DSP output → jacks | 1 (output jack) | reversed | correct | correct |

**Why one compensation can't do it.** The problem sits on both sides of the codec. To keep the
data in the middle labelled correctly (channel 0 = left in recordings, live streams, the web page
and per-channel effects), each side needs its own correction.

The only single-compensation alternative is to swap the mic input and leave the outputs straight.
It makes both jacks sound right, but every recording and stream, mic and line alike, would carry
left and right backwards, and any effect that compares the two microphones would see them swapped.

## Evidence

**Measured:**
- **2026-07-08, firmware with every compensation removed** (R63 straight, DSP unpack
  textbook):
  - A channel-tagged WAV put the right channel's tone in the **left** ear.
  - Mic to headphone was swapped.
  - A line-out → line-in loopback came back **unswapped**, because the two jack swaps cancel.
    It was read at the time as "TX quirk plus an unexplained line-in crossing".
- **2026-08-10, Rev 3 DVT** (U1): "Headphone-out L/R CONFIRMED SWAPPED … swapped through DUT
  line-in → headphone out" (session notes, YouTube L/R test).
- **2026-09-27, Rev A:** the PC-left tone (plug tip) arrives on IN3R, the right amplifier. The
  board tester has measured line-in as "swapped" on every Rev A board since; its expected value
  says so.
- **2026-10-01, issue #20** (Dan, SN0192, 0.9.1-103): in `input diff`, headphone and line out
  are left/right backwards; in `input mic` they are correct.
- **Capture side:** live mic captures land left mic → channel 0. The ESP32-S3 I2S receive is
  textbook (the "slot 1 = left" note from issue #1 is wrong).

**Footprint geometry (read from the KiCad footprint):**
- Plug opening at x = −8.6 mm.
- Pad 3 (sleeve) 5.1 mm in, pad 2 7.1 mm (ring zone), pad 4 12.1 mm (tip zone).
- The schematic puts L on pad 2 and R on pad 4, so tip lands on the R net.

## Why it looked "swapped again": it was never fixed

Line in → outputs has been reversed **continuously since 2026-06-11**, on every board revision
since:
- The diff-mode routing (P1 R52=0x10, R54=0x10, R55=0x04, R57=0x04) is unchanged since March
  2026 (703ad01 / c83f975).
- The R63 output cross dates from 2026-06-11.
- Nothing since then, including the 2026-09-28 LINE IN fix, touched either.

What the repeated L/R checks verified, and what they left open:

| Date | Board | What was checked | Line-in finding | What happened |
|---|---|---|---|---|
| 2026-06-11 | Rev 2 | mic → ear (issue #1) | not checked | R63 cross added |
| 2026-07-08 | Rev 2 (SE line in) | full chain, with compensations removed one by one | "one residual inversion not yet localized" in the line-out → line-in loopback (83e72db commit message) | left "under investigation" |
| 2026-08-10 | Rev 3 U1 (diff) | DVT YouTube L/R test | "Headphone-out L/R CONFIRMED SWAPPED … through DUT line-in" | DVT open finding; localization probe "pending" |
| 2026-09-27 | Rev A | board tester | line-in measured swapped on every board | **recorded as a known design issue and written into the tester as the expected value, instead of fixed** |
| 2026-10-01 | Rev A SN0192 | Dan, by ear | line-in L/R backwards at headphone and line out | issue #20 |

The mic path, the output path and the capture labels passed every time, and they are still
correct. The line-in path was flagged at each look and deferred each time.

## Firmware as it stands

| Item | Where | Notes |
|---|---|---|
| DAC cross-map, std I2S | `audio_codec.c`: `CODEC_R63_DAC_ON` = 0xE8 | Its comment still says "TX-quirk comp". Under the footprint explanation it compensates the output jacks. |
| DAC mapping in TDM | `audio_codec.c`: `s_r63_straight = tdm` | TDM was set **straight** on the TX-quirk theory. If the footprint is the cause, TDM outputs are reversed at the jacks. **Never measured.** |
| Line-in swap | `rx_fill_task` → `line_in_swap_lr()` in `radiosonic_main.c`; flag `LINE_IN_LR_SWAPPED` | New 2026-10-01. Runs in line-in mode only, before the input stats, recorder and DSP. Committed; not yet run on a board. |
| Board tester | `tools/board_test/rs_board_test.py`: `line_in_swapped` | Expects **straight** from 2026-10-01. It ignores the `true` saved in older `rs_board_test_config.json` files, because the mapping belongs to the firmware it flashes. |

## Settling it, and removing both compensations

1. **Continuity, about 30 s per jack.**
   - Plug a TRS cable into J2.
   - With a DMM, check whether the plug **tip** connects to pad 4 (the net the schematic calls
     R) or to pad 2.
   - Do the same on J1 and J3.
   - Tip on pad 4 confirms the footprint as the cause.
2. **Optional:** check the PJ-3200 datasheet pinout against the footprint.
3. **Next PCB revision** (hardware issue #38): fix the footprint (or the symbol's pin map) so the tip lands on the L
   net, on all three jacks. For that revision:
   - set `LINE_IN_LR_SWAPPED` to 0
   - make R63 straight, ideally by moving the R63 choice into the board header as a flag like
     `LINE_IN_LR_SWAPPED`, so both compensations are defined together per revision
4. **Before relying on TDM:** measure its output mapping (headphone L/R with a channel-tagged
   source).

## Superseded explanations

Kept so nobody re-derives them.

- **2026-06-11 (10c2b6c):** "the Rev 2 PCB crosses HPL/HPR and LOL/LOR". Retracted 2026-07-08:
  the schematic and layout are straight, and the swap also shows on Rev 1. (Both readings are
  consistent with a jack footprint shared by every revision.)
- **2026-07-08 (83e72db) and 2026-09-05:** "the ESP32-S3 I2S TX sends the first memory word in
  the WS-high half" (a silicon quirk), plus a separate, never-located "analog crossing in the
  line-in leg".
  - It rests on driver-source reading and the same listening tests as above.
  - The footprint explains both observations with one cause.
  - It is not formally refuted. A scope check would do that: WCLK vs DIN while playing
    left-only audio.
- **Issue #1:** "slot 1 = left". Refuted 2026-07-08: RX is textbook Philips I2S.

Related:
- [Firmware issue #1](https://github.com/sigprolabs/RadioSonic-Firmware/issues/1): mic L/R, closed.
- [Firmware issue #20](https://github.com/sigprolabs/RadioSonic-Firmware/issues/20): line-in L/R.
- [Hardware issue #38](https://github.com/sigprolabs/RadioSonic-Hardware/issues/38): the jack footprint itself, to fix on the next PCB revision.
